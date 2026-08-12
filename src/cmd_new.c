#include "common.h"
#include "cli.h"

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>

// ---- Directory creation helpers ----

static int make_dir(const char* path) {
    if (mkdir(path, 0755) != 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "Error: Could not create directory '%s': %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int create_directories(const char* base) {
    char path[4096];

    // Create base directory
    if (make_dir(base) != 0) return -1;

    // routes/
    snprintf(path, sizeof(path), "%s/routes", base);
    if (make_dir(path) != 0) return -1;

    // views/
    snprintf(path, sizeof(path), "%s/views", base);
    if (make_dir(path) != 0) return -1;

    // views/layouts/
    snprintf(path, sizeof(path), "%s/views/layouts", base);
    if (make_dir(path) != 0) return -1;

    // public/
    snprintf(path, sizeof(path), "%s/public", base);
    if (make_dir(path) != 0) return -1;

    // public/css/
    snprintf(path, sizeof(path), "%s/public/css", base);
    if (make_dir(path) != 0) return -1;

    // public/js/
    snprintf(path, sizeof(path), "%s/public/js", base);
    if (make_dir(path) != 0) return -1;

    // migrations/
    snprintf(path, sizeof(path), "%s/migrations", base);
    if (make_dir(path) != 0) return -1;

    // config/
    snprintf(path, sizeof(path), "%s/config", base);
    if (make_dir(path) != 0) return -1;

    return 0;
}

// ---- File generation helpers ----

static int write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "Error: Could not create file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

static int generate_app_ve(const char* base, const char* appname) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/app.ve", base);

    char content[2048];
    snprintf(content, sizeof(content),
        "// %s - Main application entry point\n"
        "\n"
        "import \"config/app\"\n"
        "import \"routes/index\"\n"
        "\n"
        "let app = Server.new()\n"
        "\n"
        "// Register routes\n"
        "app.use(routes)\n"
        "\n"
        "// Serve static files\n"
        "app.static(\"/public\", \"public\")\n"
        "\n"
        "// Start the server\n"
        "let port = config.port || 3000\n"
        "app.listen(port, fn() {\n"
        "  println(\"Server running at http://localhost:\" + str(port))\n"
        "})\n",
        appname
    );

    return write_file(path, content);
}

static int generate_routes_index(const char* base) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/routes/index.ve", base);

    const char* content =
        "// Route definitions\n"
        "\n"
        "let routes = Router.new()\n"
        "\n"
        "routes.get(\"/\", fn(req, res) {\n"
        "  res.render(\"index\", { title: \"Welcome\" })\n"
        "})\n"
        "\n"
        "export routes\n";

    return write_file(path, content);
}

static int generate_views_layout(const char* base, const char* appname) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/views/layouts/main.ve", base);

    char content[2048];
    snprintf(content, sizeof(content),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "  <title>{{ title }} - %s</title>\n"
        "  <link rel=\"stylesheet\" href=\"/public/css/style.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <main>\n"
        "    {{@ body }}\n"
        "  </main>\n"
        "  <script src=\"/public/js/app.js\"></script>\n"
        "</body>\n"
        "</html>\n",
        appname
    );

    return write_file(path, content);
}

static int generate_views_index(const char* base) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/views/index.ve", base);

    const char* content =
        "{{> layouts/main }}\n"
        "\n"
        "<h1>Welcome to your Vek app!</h1>\n"
        "<p>Edit this file at views/index.ve to get started.</p>\n";

    return write_file(path, content);
}

static int generate_css(const char* base) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/public/css/style.css", base);

    const char* content =
        "/* CSS Reset */\n"
        "*, *::before, *::after {\n"
        "  box-sizing: border-box;\n"
        "}\n"
        "\n"
        "* {\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "}\n"
        "\n"
        "body {\n"
        "  font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto,\n"
        "    Oxygen, Ubuntu, Cantarell, sans-serif;\n"
        "  line-height: 1.6;\n"
        "  color: #333;\n"
        "  max-width: 800px;\n"
        "  margin: 0 auto;\n"
        "  padding: 2rem;\n"
        "}\n"
        "\n"
        "h1 {\n"
        "  margin-bottom: 1rem;\n"
        "}\n";

    return write_file(path, content);
}

static int generate_config(const char* base) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/config/app.ve", base);

    const char* content =
        "// Application configuration\n"
        "\n"
        "let config = {\n"
        "  port: 3000,\n"
        "  env: \"development\",\n"
        "  name: \"vek-app\"\n"
        "}\n"
        "\n"
        "export config\n";

    return write_file(path, content);
}

// ---- Public entry point ----

int cmd_new_run(int argc, char** argv) {
    bool no_prompt = cli_has_flag(argc, argv, "--no-prompt");
    bool color = cli_color_enabled();

    // Find the project name argument (skip flags)
    const char* appname_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] != '-') {
            appname_path = argv[i];
            break;
        }
    }

    if (appname_path == NULL) {
        fprintf(stderr, "Error: project name is required.\n");
        fprintf(stderr, "Usage: vek new <project-name> [--no-prompt]\n");
        return 64; // EX_USAGE
    }

    // Extract the basename for display purposes
    // We need a mutable copy for basename()
    char name_buf[4096];
    snprintf(name_buf, sizeof(name_buf), "%s", appname_path);
    const char* appname = basename(name_buf);

    // Check if directory already exists
    struct stat st;
    if (stat(appname_path, &st) == 0) {
        if (no_prompt) {
            fprintf(stderr, "Error: directory '%s' already exists.\n", appname_path);
            return 1;
        } else {
            fprintf(stderr, "Warning: directory '%s' already exists. Aborting.\n", appname_path);
            return 1;
        }
    }

    // Create directory structure
    if (create_directories(appname_path) != 0) {
        return 74; // EX_IOERR
    }

    // Generate starter files
    if (generate_app_ve(appname_path, appname) != 0) return 74;
    if (generate_routes_index(appname_path) != 0) return 74;
    if (generate_views_layout(appname_path, appname) != 0) return 74;
    if (generate_views_index(appname_path) != 0) return 74;
    if (generate_css(appname_path) != 0) return 74;
    if (generate_config(appname_path) != 0) return 74;

    // Print success message
    if (color) {
        printf("\n%s%sProject '%s' created successfully!%s\n\n", CLI_BOLD, CLI_GREEN, appname, CLI_RESET);
        printf("  Next steps:\n\n");
        printf("    %scd %s && vek dev%s\n\n", CLI_GREEN, appname_path, CLI_RESET);
    } else {
        printf("\nProject '%s' created successfully!\n\n", appname);
        printf("  Next steps:\n\n");
        printf("    cd %s && vek dev\n\n", appname_path);
    }

    return 0;
}
