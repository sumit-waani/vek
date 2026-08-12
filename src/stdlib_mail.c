#define _GNU_SOURCE

#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

// External base64_encode from stdlib_session.c
extern char* base64_encode(const uint8_t* data, size_t len, size_t* out_len);

// ---- SMTP helpers ----

static int smtp_connect(const char* host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static bool smtp_read_response(int fd, char* buf, size_t buf_size) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 10000); // 10 second timeout
    if (ret <= 0) return false;

    ssize_t n = recv(fd, buf, buf_size - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

static bool smtp_send_cmd(int fd, const char* cmd) {
    size_t len = strlen(cmd);
    ssize_t sent = send(fd, cmd, len, 0);
    return sent == (ssize_t)len;
}

static bool smtp_send_and_check(int fd, const char* cmd, char expected_first) {
    if (!smtp_send_cmd(fd, cmd)) return false;
    char buf[512];
    if (!smtp_read_response(fd, buf, sizeof(buf))) return false;
    return buf[0] == expected_first;
}

// Get string value from config map
static const char* config_get_str(ObjMap* config, const char* key, size_t* out_len) {
    ObjString* k = obj_string_new(key, (uint32_t)strlen(key));
    Value val;
    if (obj_map_get(config, k, &val) && IS_STRING(val)) {
        ObjString* s = AS_STRING(val);
        if (out_len) *out_len = s->length;
        return s->data;
    }
    if (out_len) *out_len = 0;
    return NULL;
}

static int config_get_int(ObjMap* config, const char* key, int def) {
    ObjString* k = obj_string_new(key, (uint32_t)strlen(key));
    Value val;
    if (obj_map_get(config, k, &val) && IS_INT(val)) {
        return (int)AS_INT(val);
    }
    return def;
}

// mail.send(config_map)
// config: {to, from, subject, text, html, host, port, username, password}
static Value native_mail_send(int argc, Value* args) {
    (void)argc;
    if (!IS_MAP(args[0])) return VAL_NIL;

    ObjMap* config = AS_MAP(args[0]);

    const char* to = config_get_str(config, "to", NULL);
    const char* from = config_get_str(config, "from", NULL);
    const char* subject = config_get_str(config, "subject", NULL);
    const char* text = config_get_str(config, "text", NULL);
    const char* html = config_get_str(config, "html", NULL);
    const char* host = config_get_str(config, "host", NULL);
    int port = config_get_int(config, "port", 25);
    const char* username = config_get_str(config, "username", NULL);
    const char* password = config_get_str(config, "password", NULL);

    if (!to || !from || !host) return VAL_NIL;
    if (!text && !html) return VAL_NIL;

    // Connect
    int fd = smtp_connect(host, port);
    if (fd < 0) return VAL_NIL;

    char buf[512];

    // Read greeting
    if (!smtp_read_response(fd, buf, sizeof(buf)) || buf[0] != '2') {
        close(fd);
        return VAL_NIL;
    }

    // EHLO
    char ehlo[300];
    snprintf(ehlo, sizeof(ehlo), "EHLO localhost\r\n");
    if (!smtp_send_and_check(fd, ehlo, '2')) {
        close(fd);
        return VAL_NIL;
    }

    // AUTH PLAIN if credentials provided
    if (username && password) {
        // AUTH PLAIN: base64("\0username\0password")
        size_t ulen = strlen(username);
        size_t plen = strlen(password);
        size_t auth_raw_len = 1 + ulen + 1 + plen;
        uint8_t* auth_raw = (uint8_t*)malloc(auth_raw_len);
        auth_raw[0] = '\0';
        memcpy(auth_raw + 1, username, ulen);
        auth_raw[1 + ulen] = '\0';
        memcpy(auth_raw + 2 + ulen, password, plen);

        size_t b64_len = 0;
        char* b64 = base64_encode(auth_raw, auth_raw_len, &b64_len);
        free(auth_raw);

        char auth_cmd[1024];
        snprintf(auth_cmd, sizeof(auth_cmd), "AUTH PLAIN %s\r\n", b64);
        free(b64);

        if (!smtp_send_and_check(fd, auth_cmd, '2')) {
            close(fd);
            return VAL_NIL;
        }
    }

    // MAIL FROM
    char mail_from[512];
    snprintf(mail_from, sizeof(mail_from), "MAIL FROM:<%s>\r\n", from);
    if (!smtp_send_and_check(fd, mail_from, '2')) {
        close(fd);
        return VAL_NIL;
    }

    // RCPT TO
    char rcpt_to[512];
    snprintf(rcpt_to, sizeof(rcpt_to), "RCPT TO:<%s>\r\n", to);
    if (!smtp_send_and_check(fd, rcpt_to, '2')) {
        close(fd);
        return VAL_NIL;
    }

    // DATA
    if (!smtp_send_and_check(fd, "DATA\r\n", '3')) {
        close(fd);
        return VAL_NIL;
    }

    // Send message headers + body
    char headers_buf[2048];
    int hlen = snprintf(headers_buf, sizeof(headers_buf),
        "From: %s\r\n"
        "To: %s\r\n"
        "Subject: %s\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/%s; charset=UTF-8\r\n"
        "\r\n",
        from, to, subject ? subject : "(no subject)",
        html ? "html" : "plain");

    smtp_send_cmd(fd, headers_buf);

    const char* body = html ? html : text;
    smtp_send_cmd(fd, body);

    // End DATA
    if (!smtp_send_and_check(fd, "\r\n.\r\n", '2')) {
        close(fd);
        return VAL_NIL;
    }

    // QUIT
    smtp_send_cmd(fd, "QUIT\r\n");
    (void)hlen;

    close(fd);
    return VAL_TRUE;
}

void stdlib_mail_init(ObjMap* pkg) {
    stdlib_register(pkg, "send", native_mail_send, 1);
}
