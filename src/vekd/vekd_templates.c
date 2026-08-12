/*
 * vekd_templates.c - Server-side HTML rendering for the vekd web dashboard.
 *
 * All pages are rendered as C string building with embedded htmx attributes.
 * CSS is inlined in the layout for zero external dependencies.
 */
#include "vekd_templates.h"
#include "vekd_config.h"
#include "../sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- Buffer helpers ---- */

void vekd_buf_init(VekdBuf *buf, size_t initial_cap) {
    buf->data = malloc(initial_cap);
    buf->len = 0;
    buf->cap = initial_cap;
    if (buf->data) buf->data[0] = '\0';
}

static void vekd_buf_grow(VekdBuf *buf, size_t needed) {
    if (buf->len + needed + 1 <= buf->cap) return;
    size_t new_cap = buf->cap * 2;
    if (new_cap < buf->len + needed + 1) new_cap = buf->len + needed + 1;
    char *new_data = realloc(buf->data, new_cap);
    if (!new_data) return;
    buf->data = new_data;
    buf->cap = new_cap;
}

void vekd_buf_append(VekdBuf *buf, const char *str) {
    if (!str || !buf->data) return;
    size_t slen = strlen(str);
    vekd_buf_grow(buf, slen);
    memcpy(buf->data + buf->len, str, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
}

void vekd_buf_printf(VekdBuf *buf, const char *fmt, ...) {
    if (!buf->data) return;
    va_list args;
    va_start(args, fmt);
    /* First pass: measure needed size */
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) { va_end(args); return; }

    vekd_buf_grow(buf, (size_t)needed);
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, args);
    buf->len += (size_t)needed;
    va_end(args);
}

void vekd_buf_append_escaped(VekdBuf *buf, const char *str) {
    if (!str || !buf->data) return;
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '<':  vekd_buf_append(buf, "&lt;"); break;
            case '>':  vekd_buf_append(buf, "&gt;"); break;
            case '&':  vekd_buf_append(buf, "&amp;"); break;
            case '"':  vekd_buf_append(buf, "&quot;"); break;
            case '\'': vekd_buf_append(buf, "&#39;"); break;
            default:   vekd_buf_grow(buf, 1);
                       buf->data[buf->len++] = *p;
                       buf->data[buf->len] = '\0';
                       break;
        }
    }
}

void vekd_buf_free(VekdBuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/* ---- CSS ---- */

static const char *CSS =
    "<style>"
    "*, *::before, *::after { box-sizing: border-box; }"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
    " margin: 0; padding: 0; background: #f5f5f5; color: #333; }"
    "a { color: #2563eb; text-decoration: none; }"
    "a:hover { text-decoration: underline; }"
    ".container { max-width: 1100px; margin: 0 auto; padding: 20px; }"
    "nav { background: #1e293b; color: #fff; padding: 12px 20px; display: flex;"
    " align-items: center; justify-content: space-between; }"
    "nav .brand { font-size: 1.3em; font-weight: 700; color: #fff; }"
    "nav .nav-links a { color: #cbd5e1; margin-left: 20px; }"
    "nav .nav-links a:hover { color: #fff; }"
    ".card { background: #fff; border-radius: 8px; padding: 24px;"
    " margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }"
    ".btn { display: inline-block; padding: 8px 16px; border-radius: 6px;"
    " border: none; cursor: pointer; font-size: 0.9em; font-weight: 500; }"
    ".btn-primary { background: #2563eb; color: #fff; }"
    ".btn-primary:hover { background: #1d4ed8; }"
    ".btn-danger { background: #dc2626; color: #fff; }"
    ".btn-danger:hover { background: #b91c1c; }"
    ".btn-success { background: #16a34a; color: #fff; }"
    ".btn-success:hover { background: #15803d; }"
    ".btn-sm { padding: 4px 10px; font-size: 0.8em; }"
    "input, select, textarea { padding: 8px 12px; border: 1px solid #d1d5db;"
    " border-radius: 6px; font-size: 0.9em; width: 100%; margin-bottom: 12px; }"
    "label { display: block; font-weight: 500; margin-bottom: 4px; font-size: 0.9em; }"
    ".form-group { margin-bottom: 16px; }"
    "table { width: 100%; border-collapse: collapse; }"
    "th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #e5e7eb; }"
    "th { font-weight: 600; font-size: 0.85em; color: #6b7280; text-transform: uppercase; }"
    ".badge { display: inline-block; padding: 2px 8px; border-radius: 12px;"
    " font-size: 0.75em; font-weight: 600; }"
    ".badge-running { background: #dcfce7; color: #166534; }"
    ".badge-stopped { background: #fee2e2; color: #991b1b; }"
    ".badge-pending { background: #fef3c7; color: #92400e; }"
    ".badge-crashed { background: #fecaca; color: #7f1d1d; }"
    ".stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));"
    " gap: 16px; margin-bottom: 24px; }"
    ".stat-card { background: #fff; border-radius: 8px; padding: 20px;"
    " box-shadow: 0 1px 3px rgba(0,0,0,0.1); }"
    ".stat-card h3 { margin: 0 0 4px; font-size: 0.85em; color: #6b7280; }"
    ".stat-card .value { font-size: 1.8em; font-weight: 700; color: #1e293b; }"
    ".alert { padding: 12px 16px; border-radius: 6px; margin-bottom: 16px; }"
    ".alert-error { background: #fef2f2; color: #991b1b; border: 1px solid #fecaca; }"
    ".alert-success { background: #f0fdf4; color: #166534; border: 1px solid #bbf7d0; }"
    ".log-output { background: #1e293b; color: #e2e8f0; font-family: monospace;"
    " padding: 16px; border-radius: 8px; max-height: 400px; overflow-y: auto;"
    " white-space: pre-wrap; font-size: 0.85em; }"
    ".login-box { max-width: 400px; margin: 80px auto; }"
    ".tabs { display: flex; border-bottom: 2px solid #e5e7eb; margin-bottom: 20px; }"
    ".tab { padding: 8px 16px; cursor: pointer; border-bottom: 2px solid transparent;"
    " margin-bottom: -2px; font-weight: 500; color: #6b7280; }"
    ".tab.active { border-bottom-color: #2563eb; color: #2563eb; }"
    ".env-row { display: flex; gap: 8px; align-items: center; margin-bottom: 8px; }"
    ".env-row input { margin-bottom: 0; }"
    ".htmx-indicator { opacity: 0; transition: opacity 200ms ease-in; }"
    ".htmx-request .htmx-indicator { opacity: 1; }"
    "</style>";

/* ---- Inline htmx (minimal implementation) ---- */

static const char *HTMX_INLINE =
    "/*htmx-mini*/"
    "(function(){"
    "var htmx={};"
    "function qsa(s,r){return(r||document).querySelectorAll(s);}"
    "function qs(s,r){return(r||document).querySelector(s);}"
    "function ajax(m,u,b,cb){"
    "  var x=new XMLHttpRequest();"
    "  x.open(m,u,true);"
    "  x.setRequestHeader('HX-Request','true');"
    "  if(b)x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
    "  x.onload=function(){"
    "    var redir=x.getResponseHeader('HX-Redirect');"
    "    if(redir){window.location=redir;return;}"
    "    cb(x.responseText,x.status);"
    "  };"
    "  x.send(b||null);"
    "}"
    "function getFormData(el){"
    "  var form=el.closest('form');"
    "  if(!form)return null;"
    "  var fd=new FormData(form);"
    "  var parts=[];"
    "  fd.forEach(function(v,k){parts.push(encodeURIComponent(k)+'='+encodeURIComponent(v));});"
    "  return parts.join('&');"
    "}"
    "function getTarget(el){"
    "  var t=el.getAttribute('hx-target');"
    "  if(t)return qs(t);"
    "  return el;"
    "}"
    "function doSwap(target,html,mode){"
    "  if(!target)return;"
    "  if(mode==='none')return;"
    "  if(mode==='innerHTML'||!mode){target.innerHTML=html;}"
    "  else if(mode==='outerHTML'){target.outerHTML=html;}"
    "  process(target);"
    "}"
    "function trigger(el){"
    "  var method=null,url=null;"
    "  if(el.hasAttribute('hx-get')){method='GET';url=el.getAttribute('hx-get');}"
    "  else if(el.hasAttribute('hx-post')){method='POST';url=el.getAttribute('hx-post');}"
    "  else if(el.hasAttribute('hx-delete')){method='DELETE';url=el.getAttribute('hx-delete');}"
    "  if(!method)return;"
    "  var confirm=el.getAttribute('hx-confirm');"
    "  if(confirm&&!window.confirm(confirm))return;"
    "  var swap=el.getAttribute('hx-swap')||'innerHTML';"
    "  var target=getTarget(el);"
    "  var body=null;"
    "  if(method==='POST'){body=getFormData(el);}"
    "  el.classList.add('htmx-request');"
    "  ajax(method,url,body,function(html){"
    "    el.classList.remove('htmx-request');"
    "    doSwap(target,html,swap);"
    "  });"
    "}"
    "function process(root){"
    "  var els=qsa('[hx-get],[hx-post],[hx-delete]',root);"
    "  for(var i=0;i<els.length;i++){"
    "    var el=els[i];"
    "    if(el._htmx)continue;"
    "    el._htmx=true;"
    "    var trig=el.getAttribute('hx-trigger')||'click';"
    "    if(trig.indexOf('every ')===0){"
    "      (function(e,ms){"
    "        setInterval(function(){trigger(e);},ms);"
    "      })(el,parseInt(trig.replace('every ','').replace('s',''))*1000);"
    "    }else if(trig==='load'){"
    "      (function(e){setTimeout(function(){trigger(e);},0);})(el);"
    "    }else if(trig.indexOf('load,')===0){"
    "      (function(e,rest){"
    "        setTimeout(function(){trigger(e);},0);"
    "        var parts=rest.split(',');"
    "        for(var j=0;j<parts.length;j++){"
    "          var p=parts[j].trim();"
    "          if(p.indexOf('every ')===0){"
    "            var ms=parseInt(p.replace('every ','').replace('s',''))*1000;"
    "            setInterval(function(){trigger(e);},ms);"
    "          }"
    "        }"
    "      })(el,trig.substring(5));"
    "    }else if(trig==='submit'){"
    "      el.addEventListener('submit',function(ev){ev.preventDefault();trigger(this);});"
    "    }else{"
    "      el.addEventListener(trig,function(ev){"
    "        if(this.tagName==='FORM')ev.preventDefault();"
    "        trigger(this);"
    "      });"
    "    }"
    "  }"
    "}"
    "document.addEventListener('DOMContentLoaded',function(){process(document);});"
    "window.htmx=htmx;"
    "})();";

/* ---- Layout ---- */

void vekd_tpl_layout(VekdBuf *buf, const char *title, const char *content,
                     bool logged_in) {
    vekd_buf_append(buf, "<!DOCTYPE html><html lang=\"en\"><head>");
    vekd_buf_append(buf, "<meta charset=\"UTF-8\">");
    vekd_buf_append(buf, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
    vekd_buf_printf(buf, "<title>%s - vekd</title>", title);
    vekd_buf_append(buf, CSS);
    vekd_buf_append(buf, "<script>");
    vekd_buf_append(buf, HTMX_INLINE);
    vekd_buf_append(buf, "</script>");
    vekd_buf_append(buf, "</head><body>");

    if (logged_in) {
        vekd_buf_append(buf, "<nav>");
        vekd_buf_append(buf, "<span class=\"brand\">vekd</span>");
        vekd_buf_append(buf, "<div class=\"nav-links\">");
        vekd_buf_append(buf, "<a href=\"/dashboard\">Dashboard</a>");
        vekd_buf_append(buf, "<a href=\"/apps/new\">Deploy</a>");
        vekd_buf_append(buf, "<a href=\"/settings/users\">Settings</a>");
        vekd_buf_append(buf, "<a href=\"/logout\">Logout</a>");
        vekd_buf_append(buf, "</div></nav>");
    }

    vekd_buf_append(buf, "<div class=\"container\">");
    vekd_buf_append(buf, content);
    vekd_buf_append(buf, "</div></body></html>");
}

/* ---- Login Page ---- */

char *vekd_tpl_login(const char *error_msg) {
    VekdBuf content;
    vekd_buf_init(&content, 4096);

    vekd_buf_append(&content, "<div class=\"login-box\">");
    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h2 style=\"margin-top:0\">vekd Login</h2>");

    if (error_msg && error_msg[0]) {
        vekd_buf_append(&content, "<div class=\"alert alert-error\">");
        vekd_buf_append_escaped(&content, error_msg);
        vekd_buf_append(&content, "</div>");
    }

    vekd_buf_append(&content, "<form method=\"POST\" action=\"/login\">");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"email\">Email</label>");
    vekd_buf_append(&content, "<input type=\"email\" id=\"email\" name=\"email\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"password\">Password</label>");
    vekd_buf_append(&content, "<input type=\"password\" id=\"password\" name=\"password\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary\" style=\"width:100%\">Sign In</button>");
    vekd_buf_append(&content, "</form>");
    vekd_buf_append(&content, "</div></div>");

    VekdBuf page;
    vekd_buf_init(&page, 8192);
    vekd_tpl_layout(&page, "Login", content.data, false);
    vekd_buf_free(&content);

    return page.data;
}

/* ---- Dashboard ---- */

char *vekd_tpl_dashboard(VekdDB *db) {
    VekdBuf content;
    vekd_buf_init(&content, 8192);

    int app_count = vekd_db_app_count(db);

    /* Stats */
    vekd_buf_append(&content, "<div class=\"stats\">");
    vekd_buf_append(&content, "<div class=\"stat-card\">");
    vekd_buf_append(&content, "<h3>Total Apps</h3>");
    vekd_buf_printf(&content, "<div class=\"value\">%d</div>", app_count >= 0 ? app_count : 0);
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<div class=\"stat-card\">");
    vekd_buf_append(&content, "<h3>Version</h3>");
    vekd_buf_printf(&content, "<div class=\"value\">%s</div>", VEKD_VERSION);
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "</div>");

    /* App list */
    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:16px\">");
    vekd_buf_append(&content, "<h2 style=\"margin:0\">Applications</h2>");
    vekd_buf_append(&content, "<a href=\"/apps/new\" class=\"btn btn-primary\">Deploy New App</a>");
    vekd_buf_append(&content, "</div>");

    vekd_buf_append(&content, "<div id=\"app-list\" hx-get=\"/apps/list\" hx-trigger=\"every 5s\">");

    /* Render initial app list */
    char *list = vekd_tpl_app_list_partial(db);
    if (list) {
        vekd_buf_append(&content, list);
        free(list);
    }

    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 16384);
    vekd_tpl_layout(&page, "Dashboard", content.data, true);
    vekd_buf_free(&content);

    return page.data;
}

/* ---- App list partial ---- */

char *vekd_tpl_app_list_partial(VekdDB *db) {
    VekdBuf buf;
    vekd_buf_init(&buf, 4096);

    /* Query all apps */
    const char *sql = "SELECT id, name, state, port, domain FROM apps ORDER BY name";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        vekd_buf_append(&buf, "<p>Error loading apps.</p>");
        return buf.data;
    }

    int count = 0;
    vekd_buf_append(&buf, "<table><thead><tr>");
    vekd_buf_append(&buf, "<th>Name</th><th>Status</th><th>Port</th><th>Domain</th><th>Actions</th>");
    vekd_buf_append(&buf, "</tr></thead><tbody>");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *state = (const char *)sqlite3_column_text(stmt, 2);
        int port = sqlite3_column_int(stmt, 3);
        const char *domain = (const char *)sqlite3_column_text(stmt, 4);

        const char *badge_class = "badge-pending";
        if (state && strcmp(state, "running") == 0) badge_class = "badge-running";
        else if (state && strcmp(state, "stopped") == 0) badge_class = "badge-stopped";
        else if (state && strcmp(state, "crashed") == 0) badge_class = "badge-crashed";

        vekd_buf_append(&buf, "<tr>");
        vekd_buf_printf(&buf, "<td><a href=\"/apps/%lld\">", (long long)id);
        vekd_buf_append_escaped(&buf, name ? name : "");
        vekd_buf_append(&buf, "</a></td>");
        vekd_buf_printf(&buf, "<td><span class=\"badge %s\">%s</span></td>",
                        badge_class, state ? state : "unknown");
        vekd_buf_printf(&buf, "<td>%d</td>", port);
        vekd_buf_printf(&buf, "<td>%s</td>", domain ? domain : "-");
        vekd_buf_printf(&buf, "<td>");
        vekd_buf_printf(&buf,
            "<button class=\"btn btn-success btn-sm\" "
            "hx-post=\"/apps/%lld/deploy\" hx-swap=\"none\">Deploy</button> ",
            (long long)id);
        vekd_buf_printf(&buf,
            "<button class=\"btn btn-sm\" style=\"background:#64748b;color:#fff\" "
            "hx-post=\"/apps/%lld/restart\" hx-swap=\"none\">Restart</button>",
            (long long)id);
        vekd_buf_append(&buf, "</td>");
        vekd_buf_append(&buf, "</tr>");
        count++;
    }

    sqlite3_finalize(stmt);

    if (count == 0) {
        vekd_buf_free(&buf);
        vekd_buf_init(&buf, 256);
        vekd_buf_append(&buf, "<p style=\"color:#6b7280\">No apps deployed yet. "
                        "<a href=\"/apps/new\">Deploy your first app</a>.</p>");
    } else {
        vekd_buf_append(&buf, "</tbody></table>");
    }

    return buf.data;
}

/* ---- New App Form ---- */

char *vekd_tpl_app_new(const char *error_msg) {
    VekdBuf content;
    vekd_buf_init(&content, 4096);

    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h2 style=\"margin-top:0\">Deploy New Application</h2>");

    if (error_msg && error_msg[0]) {
        vekd_buf_append(&content, "<div class=\"alert alert-error\">");
        vekd_buf_append_escaped(&content, error_msg);
        vekd_buf_append(&content, "</div>");
    }

    vekd_buf_append(&content, "<form method=\"POST\" action=\"/apps\">");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"name\">App Name</label>");
    vekd_buf_append(&content, "<input type=\"text\" id=\"name\" name=\"name\" "
                    "placeholder=\"my-app\" required pattern=\"[a-z0-9-]+\">");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"repo_url\">Git Repository URL</label>");
    vekd_buf_append(&content, "<input type=\"url\" id=\"repo_url\" name=\"repo_url\" "
                    "placeholder=\"https://github.com/user/repo.git\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"branch\">Branch</label>");
    vekd_buf_append(&content, "<input type=\"text\" id=\"branch\" name=\"branch\" value=\"main\">");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary\">Create App</button>");
    vekd_buf_append(&content, " <a href=\"/dashboard\" style=\"margin-left:12px\">Cancel</a>");
    vekd_buf_append(&content, "</form>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 8192);
    vekd_tpl_layout(&page, "Deploy New App", content.data, true);
    vekd_buf_free(&content);

    return page.data;
}

/* ---- App Detail ---- */

char *vekd_tpl_app_detail(VekdDB *db, VekdApp *app) {
    VekdBuf content;
    vekd_buf_init(&content, 8192);

    /* Header */
    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<div style=\"display:flex;justify-content:space-between;align-items:center\">");
    vekd_buf_printf(&content, "<h2 style=\"margin:0\">");
    vekd_buf_append_escaped(&content, app->name);
    vekd_buf_append(&content, "</h2>");
    vekd_buf_append(&content, "<div>");
    vekd_buf_printf(&content,
        "<button class=\"btn btn-success btn-sm\" "
        "hx-post=\"/apps/%lld/deploy\" hx-swap=\"none\">Deploy</button> ",
        (long long)app->id);
    vekd_buf_printf(&content,
        "<button class=\"btn btn-sm\" style=\"background:#64748b;color:#fff\" "
        "hx-post=\"/apps/%lld/start\" hx-swap=\"none\">Start</button> ",
        (long long)app->id);
    vekd_buf_printf(&content,
        "<button class=\"btn btn-sm\" style=\"background:#f59e0b;color:#fff\" "
        "hx-post=\"/apps/%lld/stop\" hx-swap=\"none\">Stop</button> ",
        (long long)app->id);
    vekd_buf_printf(&content,
        "<button class=\"btn btn-sm\" style=\"background:#64748b;color:#fff\" "
        "hx-post=\"/apps/%lld/restart\" hx-swap=\"none\">Restart</button> ",
        (long long)app->id);
    vekd_buf_printf(&content,
        "<button class=\"btn btn-danger btn-sm\" "
        "hx-delete=\"/apps/%lld\" hx-confirm=\"Delete this app?\" "
        "hx-swap=\"none\">Delete</button>",
        (long long)app->id);
    vekd_buf_append(&content, "</div></div>");

    /* Info */
    const char *badge_class = "badge-pending";
    if (strcmp(app->state, "running") == 0) badge_class = "badge-running";
    else if (strcmp(app->state, "stopped") == 0) badge_class = "badge-stopped";
    else if (strcmp(app->state, "crashed") == 0) badge_class = "badge-crashed";

    vekd_buf_append(&content, "<table style=\"margin-top:16px\">");
    vekd_buf_printf(&content, "<tr><td><strong>Status</strong></td><td><span class=\"badge %s\">%s</span></td></tr>",
                    badge_class, app->state);
    vekd_buf_printf(&content, "<tr><td><strong>Repository</strong></td><td>");
    vekd_buf_append_escaped(&content, app->repo_url);
    vekd_buf_append(&content, "</td></tr>");
    vekd_buf_printf(&content, "<tr><td><strong>Branch</strong></td><td>%s</td></tr>", app->branch);
    vekd_buf_printf(&content, "<tr><td><strong>Port</strong></td><td>%d</td></tr>", app->port);
    if (app->domain[0]) {
        vekd_buf_printf(&content, "<tr><td><strong>Domain</strong></td><td>%s</td></tr>", app->domain);
    }
    vekd_buf_append(&content, "</table>");
    vekd_buf_append(&content, "</div>");

    /* Environment Variables */
    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h3 style=\"margin-top:0\">Environment Variables</h3>");
    vekd_buf_printf(&content,
        "<div id=\"env-vars\" hx-get=\"/apps/%lld/env\" hx-trigger=\"load\">",
        (long long)app->id);
    vekd_buf_append(&content, "<span class=\"htmx-indicator\">Loading...</span>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_printf(&content,
        "<form hx-post=\"/apps/%lld/env\" hx-target=\"#env-vars\" hx-swap=\"innerHTML\" "
        "style=\"margin-top:12px\">",
        (long long)app->id);
    vekd_buf_append(&content, "<div class=\"env-row\">");
    vekd_buf_append(&content, "<input type=\"text\" name=\"key\" placeholder=\"KEY\" style=\"width:200px\">");
    vekd_buf_append(&content, "<input type=\"text\" name=\"value\" placeholder=\"value\" style=\"flex:1\">");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary btn-sm\">Set</button>");
    vekd_buf_append(&content, "</div></form>");
    vekd_buf_append(&content, "</div>");

    /* Logs */
    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h3 style=\"margin-top:0\">Recent Events</h3>");
    vekd_buf_printf(&content,
        "<div id=\"app-logs\" hx-get=\"/apps/%lld/logs\" hx-trigger=\"load, every 2s\">",
        (long long)app->id);
    vekd_buf_append(&content, "<span class=\"htmx-indicator\">Loading...</span>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 16384);
    vekd_tpl_layout(&page, app->name, content.data, true);
    vekd_buf_free(&content);

    (void)db;
    return page.data;
}

/* ---- Logs partial ---- */

char *vekd_tpl_logs_partial(VekdDB *db, int64_t app_id) {
    VekdBuf buf;
    vekd_buf_init(&buf, 4096);

    const char *sql = "SELECT kind, message, created_at FROM events "
                      "WHERE app_id = ? ORDER BY created_at DESC LIMIT 50";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        vekd_buf_append(&buf, "<p>Error loading logs.</p>");
        return buf.data;
    }

    sqlite3_bind_int64(stmt, 1, app_id);

    vekd_buf_append(&buf, "<div class=\"log-output\">");
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *kind = (const char *)sqlite3_column_text(stmt, 0);
        const char *message = (const char *)sqlite3_column_text(stmt, 1);
        vekd_buf_printf(&buf, "[%s] ", kind ? kind : "info");
        vekd_buf_append_escaped(&buf, message ? message : "");
        vekd_buf_append(&buf, "\n");
        count++;
    }
    if (count == 0) {
        vekd_buf_append(&buf, "No events recorded yet.\n");
    }
    vekd_buf_append(&buf, "</div>");

    sqlite3_finalize(stmt);
    return buf.data;
}

/* ---- Env vars partial ---- */

char *vekd_tpl_env_partial(VekdDB *db, int64_t app_id) {
    VekdBuf buf;
    vekd_buf_init(&buf, 2048);

    const char *sql = "SELECT key FROM env_vars WHERE app_id = ? ORDER BY key";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        vekd_buf_append(&buf, "<p>Error loading environment variables.</p>");
        return buf.data;
    }

    sqlite3_bind_int64(stmt, 1, app_id);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(stmt, 0);
        vekd_buf_append(&buf, "<div class=\"env-row\">");
        vekd_buf_printf(&buf, "<code style=\"min-width:150px\">");
        vekd_buf_append_escaped(&buf, key ? key : "");
        vekd_buf_append(&buf, "</code>");
        vekd_buf_append(&buf, "<span style=\"color:#6b7280\">(encrypted)</span>");
        vekd_buf_printf(&buf,
            " <button class=\"btn btn-danger btn-sm\" "
            "hx-delete=\"/apps/%lld/env/%s\" hx-target=\"#env-vars\" "
            "hx-swap=\"innerHTML\">Remove</button>",
            (long long)app_id, key ? key : "");
        vekd_buf_append(&buf, "</div>");
        count++;
    }

    if (count == 0) {
        vekd_buf_append(&buf, "<p style=\"color:#6b7280\">No environment variables set.</p>");
    }

    sqlite3_finalize(stmt);
    return buf.data;
}

/* ---- Settings: Users ---- */

char *vekd_tpl_settings_users(VekdDB *db) {
    VekdBuf content;
    vekd_buf_init(&content, 4096);

    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h2 style=\"margin-top:0\">User Management</h2>");

    /* List users */
    const char *sql = "SELECT id, email, is_admin, created_at FROM users ORDER BY email";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        vekd_buf_append(&content, "<table><thead><tr>");
        vekd_buf_append(&content, "<th>Email</th><th>Role</th><th>Created</th>");
        vekd_buf_append(&content, "</tr></thead><tbody>");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *email = (const char *)sqlite3_column_text(stmt, 1);
            int is_admin = sqlite3_column_int(stmt, 2);
            vekd_buf_append(&content, "<tr>");
            vekd_buf_printf(&content, "<td>");
            vekd_buf_append_escaped(&content, email ? email : "");
            vekd_buf_append(&content, "</td>");
            vekd_buf_printf(&content, "<td>%s</td>", is_admin ? "Admin" : "User");
            vekd_buf_append(&content, "<td>-</td></tr>");
        }
        vekd_buf_append(&content, "</tbody></table>");
        sqlite3_finalize(stmt);
    }

    /* Add user form */
    vekd_buf_append(&content, "<h3>Add User</h3>");
    vekd_buf_append(&content, "<form method=\"POST\" action=\"/settings/users\">");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"email\">Email</label>");
    vekd_buf_append(&content, "<input type=\"email\" id=\"email\" name=\"email\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"password\">Password</label>");
    vekd_buf_append(&content, "<input type=\"password\" id=\"password\" name=\"password\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary\">Add User</button>");
    vekd_buf_append(&content, "</form>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 8192);
    vekd_tpl_layout(&page, "Users", content.data, true);
    vekd_buf_free(&content);

    return page.data;
}

/* ---- Settings: Cloudflare ---- */

char *vekd_tpl_settings_cloudflare(VekdDB *db) {
    VekdBuf content;
    vekd_buf_init(&content, 4096);

    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h2 style=\"margin-top:0\">Cloudflare Configuration</h2>");
    vekd_buf_append(&content, "<p>Configure your Cloudflare API token for automatic DNS management.</p>");

    /* Check if token is already set */
    const char *sql = "SELECT id FROM secrets WHERE name = 'cloudflare_token'";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    bool has_token = false;
    if (rc == SQLITE_OK) {
        has_token = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }

    if (has_token) {
        vekd_buf_append(&content, "<div class=\"alert alert-success\">Cloudflare API token is configured.</div>");
    }

    vekd_buf_append(&content, "<form method=\"POST\" action=\"/settings/cloudflare\">");
    vekd_buf_append(&content, "<div class=\"form-group\">");
    vekd_buf_append(&content, "<label for=\"token\">API Token</label>");
    vekd_buf_append(&content, "<input type=\"password\" id=\"token\" name=\"token\" "
                    "placeholder=\"Enter your Cloudflare API token\" required>");
    vekd_buf_append(&content, "</div>");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary\">Save Token</button>");
    vekd_buf_append(&content, "</form>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 8192);
    vekd_tpl_layout(&page, "Cloudflare", content.data, true);
    vekd_buf_free(&content);

    return page.data;
}

/* ---- Settings: Backup ---- */

char *vekd_tpl_settings_backup(void) {
    VekdBuf content;
    vekd_buf_init(&content, 2048);

    vekd_buf_append(&content, "<div class=\"card\">");
    vekd_buf_append(&content, "<h2 style=\"margin-top:0\">Backup &amp; Restore</h2>");
    vekd_buf_append(&content, "<p>Create a backup of the vekd database and configuration.</p>");
    vekd_buf_append(&content, "<form method=\"POST\" action=\"/settings/backup\">");
    vekd_buf_append(&content, "<button type=\"submit\" class=\"btn btn-primary\">Create Backup</button>");
    vekd_buf_append(&content, "</form>");
    vekd_buf_append(&content, "</div>");

    VekdBuf page;
    vekd_buf_init(&page, 4096);
    vekd_tpl_layout(&page, "Backup", content.data, true);
    vekd_buf_free(&content);

    return page.data;
}
