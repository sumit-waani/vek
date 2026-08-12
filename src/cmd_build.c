#include "common.h"
#include "cli.h"
#include "vebc_writer.h"
#include "compiler.h"
#include "vm.h"
#include "gc.h"
#include "memory.h"
#include "vek_stdlib.h"
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>

static char* build_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
    rewind(file);
    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) { fclose(file); return NULL; }
    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read < file_size) { free(buffer); fclose(file); return NULL; }
    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

static uint8_t* build_read_binary(const char* path, uint32_t* out_len) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    rewind(file);
    if (file_size < 0) { fclose(file); return NULL; }
    uint8_t* buffer = (uint8_t*)malloc((size_t)file_size);
    if (buffer == NULL) { fclose(file); return NULL; }
    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);
    if (bytes_read < (size_t)file_size) { free(buffer); return NULL; }
    *out_len = (uint32_t)bytes_read;
    return buffer;
}

static const char* get_app_name(void) {
    static char cwd[4096];
    static char name_buf[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return "app";
    snprintf(name_buf, sizeof(name_buf), "%s", cwd);
    const char* base = basename(name_buf);
    return base ? base : "app";
}

static const char* get_output_path(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--output=", 9) == 0) return argv[i] + 9;
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) return argv[i + 1];
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) return argv[i + 1];
    }
    return NULL;
}

static int scan_assets(VebcBuilder* builder, const char* base_dir, const char* rel_prefix,
                       uint32_t* asset_count) {
    DIR* dir = opendir(base_dir);
    if (!dir) return 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full_path[4096], rel_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, entry->d_name);
        if (rel_prefix[0] != '\0')
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, entry->d_name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_assets(builder, full_path, rel_path, asset_count);
        } else if (S_ISREG(st.st_mode)) {
            uint32_t data_len = 0;
            uint8_t* data = build_read_binary(full_path, &data_len);
            if (data) {
                vebc_builder_add_asset(builder, rel_path, data, data_len);
                free(data);
                (*asset_count)++;
            }
        }
    }
    closedir(dir);
    return 0;
}

static ObjFunction* compile_source_file(const char* path) {
    char* source = build_read_file(path);
    if (source == NULL) {
        fprintf(stderr, "Error: could not read '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    ObjFunction* fn = compile(source);
    free(source);
    if (fn == NULL) fprintf(stderr, "Error: compilation failed for '%s'\n", path);
    return fn;
}

int cmd_build_run(int argc, char** argv) {
    bool color = cli_color_enabled();
    struct stat st;
    if (stat("app.ve", &st) != 0) {
        fprintf(stderr, "Error: 'app.ve' not found in current directory.\n");
        fprintf(stderr, "Run this command from a vek project root.\n");
        return 1;
    }
    const char* output = get_output_path(argc, argv);
    char output_buf[4096];
    if (output == NULL) {
        const char* appname = get_app_name();
        snprintf(output_buf, sizeof(output_buf), "build/%s.vebc", appname);
        output = output_buf;
        mkdir("build", 0755);
    }
    if (color)
        printf("%s%sBuilding%s %s...\n", CLI_BOLD, CLI_CYAN, CLI_RESET, output);
    else
        printf("Building %s...\n", output);

    cli_set_args(argc, argv);
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    VebcBuilder builder;
    vebc_builder_init(&builder);
    uint32_t func_count = 0;
    bool had_error = false;

    ObjFunction* app_fn = compile_source_file("app.ve");
    if (app_fn == NULL) {
        had_error = true;
    } else {
        vebc_builder_add_function(&builder, app_fn);
        func_count++;
    }

    if (!had_error) {
        DIR* routes_dir = opendir("routes");
        if (routes_dir != NULL) {
            struct dirent* entry;
            while ((entry = readdir(routes_dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                size_t name_len = strlen(entry->d_name);
                if (name_len < 4) continue;
                if (strcmp(entry->d_name + name_len - 3, ".ve") != 0) continue;
                char route_path[4096];
                snprintf(route_path, sizeof(route_path), "routes/%s", entry->d_name);
                ObjFunction* route_fn = compile_source_file(route_path);
                if (route_fn == NULL) { had_error = true; break; }
                vebc_builder_add_function(&builder, route_fn);
                func_count++;
            }
            closedir(routes_dir);
        }
    }

    uint32_t asset_count = 0;
    if (!had_error && stat("public", &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_assets(&builder, "public", "public", &asset_count);
    }

    bool write_ok = false;
    if (!had_error) write_ok = vebc_builder_write(&builder, output);

    uint32_t output_size = 0;
    if (write_ok && stat(output, &st) == 0) output_size = (uint32_t)st.st_size;

    vebc_builder_destroy(&builder);
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    if (had_error) {
        if (color) fprintf(stderr, "\n%s%sBuild failed.%s\n", CLI_BOLD, CLI_RED, CLI_RESET);
        else fprintf(stderr, "\nBuild failed.\n");
        return 1;
    }
    if (!write_ok) {
        if (color) fprintf(stderr, "\n%s%sFailed to write output file.%s\n", CLI_BOLD, CLI_RED, CLI_RESET);
        else fprintf(stderr, "\nFailed to write output file.\n");
        return 1;
    }

    if (color) {
        printf("\n%s%sBuild successful!%s\n", CLI_BOLD, CLI_GREEN, CLI_RESET);
        printf("  Functions: %s%u%s compiled\n", CLI_GREEN, func_count, CLI_RESET);
        printf("  Assets:    %s%u%s embedded\n", CLI_GREEN, asset_count, CLI_RESET);
        printf("  Output:    %s%s%s (%u bytes)\n", CLI_GREEN, output, CLI_RESET, output_size);
    } else {
        printf("\nBuild successful!\n");
        printf("  Functions: %u compiled\n", func_count);
        printf("  Assets:    %u embedded\n", asset_count);
        printf("  Output:    %s (%u bytes)\n", output, output_size);
    }
    return 0;
}
