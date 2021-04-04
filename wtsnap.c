/*
 * Copyright (c) 2021 Carsten Hartenfels
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/scrnsaver.h>


#define ARGS_ERROR     (1 << 0)
#define ARGS_WANT_HELP (1 << 1)

static const char *args_help =
    "\n"
    "wtsnap - takes a snapshot of the name, class, title and focus\n"
    "properties of all currently open windows, the current time and\n"
    "the time since the last user interaction and writes it to an\n"
    "SQLite database for the sake of tracking what you worked on.\n"
    "\n"
    "Usage: %s [OPTIONS]\n"
    "\n"
    "Available options:\n"
    "\n"
    "    -b, -B\n"
    "        Include (-b) or exclude (-B) \"blank\" windows, i.e.\n"
    "        without a name, class or title, from being inserted.\n"
    "        If you don't need the full window tree with all parent\n"
    "        relationships intact, you can exclude these, since they\n"
    "        don't carry any useful information.\n"
    "        Default is to include them.\n"
    "\n"
    "    -d DISPLAY\n"
    "        Name of the X display to open.\n"
    "        Default is '', the default display.\n"
    "\n"
    "    -f DATABASE_FILE\n"
    "        Path to the SQLite database file to write to.\n"
    "        Default is ~/.wtsnap.db\n"
    "\n"
    "    -s SAMPLE_TIME\n"
    "        The time your snapshot encompasses in seconds.\n"
    "        Set this to the interval that you're taking snapshots.\n"
    "        Default is 60.\n"
    "\n"
    "    -h\n"
    "        Shows this help.\n"
    "\n";


typedef struct Context {
    const char   *db_name;
    bool         free_db_name;
    const char   *dpy_name;
    int          sample_time;
    bool         exclude_blanks;
    jmp_buf      env;
    sqlite3      *db;
    Display      *dpy;
    Window       root;
    int          idle_time;
    bool         tx;
    sqlite3_stmt *stmt;
    Window       focus;
    XClassHint   *ch;
    int          snapshot_id;
} Context;


#define DO_LOG() do { \
        va_list ap; \
        va_start(ap, fmt); \
        vfprintf(stderr, fmt, ap); \
        va_end(ap); \
        fputs("\n", stderr); \
    } while (0)

static noreturn void die(Context *ctx, const char *fmt, ...)
{
    DO_LOG();
    longjmp(ctx->env, 1);
}

static void warn(const char *fmt, ...)
{
    DO_LOG();
}

#ifdef NDEBUG
#   define debug(...) do { /* nothing */ } while (0)
#else
static void debug(const char *fmt, ...)
{
    fputs("[DEBUG]\n", stderr);
    DO_LOG();
    fputs("\n", stderr);
}
#endif


static void db_check_name(Context *ctx)
{
    if (!ctx->db_name) {
        debug("Constructing default db name", ctx->db_name);
        const char *home = getenv("HOME");
        if (!home) {
            die(ctx, "HOME not set, use -f to specify a database file");
        }

        const char *suffix    = "/.wtsnap.db";
        size_t     home_len   = strlen(home);
        size_t     suffix_len = strlen(suffix);

        size_t size = home_len + suffix_len + 1;
        char   *buf = malloc(size);
        if (!buf) {
            die(ctx, "Can't malloc %zu bytes for database path", size);
        }

        memcpy(buf, home, home_len);
        memcpy(buf + home_len, suffix, suffix_len + 1);
        ctx->db_name      = buf;
        ctx->free_db_name = true;
    }
    debug("Using db '%s'", ctx->db_name);
}

static void db_open(Context *ctx)
{
    debug("Opening db '%s'", ctx->db_name);
    int flags  = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int result = sqlite3_open_v2(ctx->db_name, &ctx->db, flags, NULL);
    if (result != SQLITE_OK) {
        die(ctx, "Can't open database '%s': %s", ctx->db_name,
            sqlite3_errmsg(ctx->db));
    }
}

static sqlite3 *db_close(sqlite3 *db)
{
    if (db) {
        debug("Closing database");
        int result = sqlite3_close(db);
        if (result != SQLITE_OK) {
            warn("Can't close database: %s", sqlite3_errmsg(db));
        }
    }
    return NULL;
}

static void db_exec(Context *ctx, const char *sql)
{
    debug("Executing %s", sql);
    int result = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        die(ctx, "Failed to execute statement '%s': %s",
            sql, sqlite3_errmsg(ctx->db));
    }
}

static void db_prepare(Context *ctx, const char *sql)
{
    debug("Preparing %s", sql);
    int result = sqlite3_prepare_v2(ctx->db, sql, -1, &ctx->stmt, NULL);
    if (result != SQLITE_OK) {
        die(ctx, "Failed to prepare statement '%s': %s",
            sql, sqlite3_errmsg(ctx->db));
    }
}

static void db_bind_int(Context *ctx, int index, int value)
{
    debug("Binding int value %d to parameter %d in %s",
          value, index, sqlite3_sql(ctx->stmt));
    int result = sqlite3_bind_int(ctx->stmt, index, value);
    if (result != SQLITE_OK) {
        die(ctx, "Failed to bind int value %d to parameter %d: %s",
            value, index, sqlite3_errmsg(ctx->db));
    }
}

static void db_bind_string(Context *ctx, int index, const char *value)
{
    debug("Binding string value '%s' to parameter %d in %s",
          value, index, sqlite3_sql(ctx->stmt));
    int result = sqlite3_bind_text(ctx->stmt, index, value,
                                   -1, SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        die(ctx, "Failed to bind string value '%s' to parameter %d: %s",
            value, index, sqlite3_errmsg(ctx->db));
    }
}

static void db_bind_unsigned_long_long_as_text(Context *ctx, int index,
                                               unsigned long long value)
{
    char text[64]; /* ensure this fits just in case */
    assert(sizeof(text) > (size_t) floor(log10(ULLONG_MAX)) + 1);
    snprintf(text, sizeof(text), "%llu", (unsigned long long) value);
    db_bind_string(ctx, index, text);
}

static int db_exec_stmt(Context *ctx, void (*callback)(Context *ctx))
{
    debug("Executing prepared statement %s", sqlite3_sql(ctx->stmt));

    int rows = 0;
    int result;
    while ((result = sqlite3_step(ctx->stmt)) == SQLITE_ROW) {
        ++rows;
        debug("Got row %d", rows);
        if (callback) {
            callback(ctx);
        }
    }

    if (result != SQLITE_DONE) {
        die(ctx, "Failed to execute prepared statement: %s",
            sqlite3_errmsg(ctx->db));
    }

    return rows;
}

static void db_reset_stmt(Context *ctx)
{
    debug("Resetting statement %s", sqlite3_sql(ctx->stmt));
    sqlite3_reset(ctx->stmt);
    sqlite3_clear_bindings(ctx->stmt);
}

static sqlite3_stmt *db_close_stmt(sqlite3_stmt *stmt)
{
    if (stmt) {
        debug("Finalizing statment %s", sqlite3_sql(stmt));
        sqlite3_finalize(stmt); /* Never fails. */
    }
    return NULL;
}

static void db_init(Context *ctx)
{
    db_exec(ctx, "create table if not exists snapshot (\n"
                 "    snapshot_id integer primary key not null,\n"
                 "    timestamp   text                not null,\n"
                 "    sample_time integer             not null,\n"
                 "    idle_time   integer)");
    db_exec(ctx, "create table if not exists window (\n"
                 "    snapshot_id integer not null,\n"
                 "    window_id   text    not null,\n"
                 "    parent_id   text,\n"
                 "    depth       integer not null,\n"
                 "    focused     integer not null,\n"
                 "    name        text,\n"
                 "    class       text,\n"
                 "    title       text,\n"
                 "    primary key (snapshot_id, window_id),\n"
                 "    foreign key (snapshot_id)\n"
                 "        references snapshot (snapshot_id)\n"
                 "        on delete cascade,\n"
                 "    foreign key (snapshot_id, parent_id)\n"
                 "        references window (snapshot_id, window_id)\n"
                 "        on delete set null)");
}

static void db_begin(Context *ctx)
{
    db_exec(ctx, "begin");
    ctx->tx = true;
}

static bool db_rollback(sqlite3 *db, bool tx)
{
    if (tx) {
        debug("Executing rollback");
        int result = sqlite3_exec(db, "rollback", NULL, NULL, NULL);
        if (result != SQLITE_OK) {
            warn("Failed to execute statement 'rollback': %s",
                 sqlite3_errmsg(db));
        }
    }
    return false;
}

static void db_commit(Context *ctx)
{
    if (!ctx->tx) {
        die(ctx, "Nothing to commit");
    }
    db_exec(ctx, "commit");
    ctx->tx = false;
}

static void db_set_snapshot_id(Context *ctx)
{
    ctx->snapshot_id = sqlite3_column_int(ctx->stmt, 0);
    debug("Snapshot id is %d", ctx->snapshot_id);
}

static void db_insert_snapshot(Context *ctx)
{
    db_prepare(ctx, "insert into snapshot (timestamp, sample_time, idle_time)\n"
                    "values (strftime('%Y-%m-%dT%H:%M:%S:%fZ', 'now'), ?, ?)");
    db_bind_int(ctx, 1, ctx->sample_time);
    db_bind_int(ctx, 2, ctx->idle_time);
    db_exec_stmt(ctx, NULL);
    ctx->stmt = db_close_stmt(ctx->stmt);

    db_prepare(ctx, "select last_insert_rowid()");
    int rows = db_exec_stmt(ctx, db_set_snapshot_id);
    if (rows != 1) {
        die(ctx, "Wanted 1 id row from inserting snapshot, but got %d", rows);
    }
    ctx->stmt = db_close_stmt(ctx->stmt);
}

static void db_prepare_window_insert(Context *ctx)
{
    db_prepare(ctx, "insert into window (snapshot_id, window_id,\n"
                    "                    parent_id, depth, focused,\n"
                    "                    name, class, title)\n"
                    "values(?, ?, ?, ?, ?, ?, ?, ?)");
}


static int x_handle_error(Display *dpy, XErrorEvent *event)
{
    char buf[1024];
    XGetErrorText(dpy, event->error_code, buf, sizeof(buf));
    warn("X11 error: %s", buf);
    return 0;
}

static void x_open_display(Context *ctx)
{
    if (ctx->dpy_name) {
        debug("Opening display '%s'", ctx->dpy_name);
    }
    else {
        debug("Opening default display");
    }
    ctx->dpy = XOpenDisplay(ctx->dpy_name);

    if (ctx->dpy) {
        ctx->root = XDefaultRootWindow(ctx->dpy);
    }
    else {
        if (ctx->dpy_name) {
            die(ctx, "Can't open display '%s'", ctx->dpy_name);
        }
        else {
            die(ctx, "Can't open default display");
        }
    }
}

static Display *x_close_display(Display *dpy)
{
    if (dpy) {
        debug("Closing display");
        XCloseDisplay(dpy);
    }
    return NULL;
}

static bool x_have_screensaver(Display *dpy)
{
    int event_base, error_base;
    return XScreenSaverQueryExtension(dpy, &event_base, &error_base);
}

static void x_clean_up_screensaver(Context *ctx, XScreenSaverInfo *info,
                                   const char *error)
{
    if (error) {
        warn("Can't get idle time: %s", error);
        ctx->idle_time = 0;
    }
    else {
        unsigned long max = INT_MAX;
        ctx->idle_time    = info->idle <= max ? info->idle : max;
        debug("Idle time: %d ms", ctx->idle_time);
    }
    if (info) {
        XFree(info);
    }
}

static void x_get_idle_time(Context *ctx)
{
    debug("Getting idle time");

    if (!x_have_screensaver(ctx->dpy)) {
        x_clean_up_screensaver(ctx, NULL, "XScreenSaver not supported");
        return;
    }

    XScreenSaverInfo *info = XScreenSaverAllocInfo();
    if (!info) {
        x_clean_up_screensaver(ctx, NULL, "Can't allocate screensaver info");
    }

    if (XScreenSaverQueryInfo(ctx->dpy, ctx->root, info) >= Success) {
        x_clean_up_screensaver(ctx, info, NULL);
    }
    else {
        x_clean_up_screensaver(ctx, info, "Querying screen saver info failed");
    }
}

static void x_get_focused_window(Context *ctx)
{
    debug("Getting input focus");
    int revert;
    if (XGetInputFocus(ctx->dpy, &ctx->focus, &revert) >= Success) {
        debug("Input focus is window %llu", (unsigned long long) ctx->focus);
    } else {
        warn("Can't get input focus");
        ctx->focus = None;
    }
}

static void x_alloc_class_hint(Context *ctx)
{
    debug("Allocating X class hint");
    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ctx->ch = ch;
    }
    else {
        die(ctx, "Can't allocate class hint structure");
    }
}

static XClassHint *x_free_class_hint(XClassHint *ch)
{
    if (ch) {
        if (ch->res_name) {
            XFree(ch->res_name);
        }
        if (ch->res_class) {
            XFree(ch->res_class);
        }
        XFree(ch);
    }
    return NULL;
}

static char *x_copy_string_property_value(const char *prop_name, void *src)
{
    size_t size = strlen((char *)src) + 1;
    char   *dst = malloc(size);
    if (dst) {
        memcpy(dst, src, size);
    }
    else {
        warn("Can't malloc %zu bytes for '%s' property value",
             size, prop_name);
    }
    return dst;
}

/*
 * This is inspired by the way dwm gets the title for a window.
 * See https://dwm.suckless.org/
 */
static char *x_get_string_property(Context *ctx, Window window,
                                   const char *prop_name)
{
    debug("Getting string property '%s'", prop_name);
    Atom prop = XInternAtom(ctx->dpy, prop_name, false);

    XTextProperty xtp;
    xtp.value      = NULL;
    char **strings = NULL;
    char *out      = NULL;

    if (XGetTextProperty(ctx->dpy, window, &xtp, prop) < Success) {
        debug("Can't get string property '%s'", prop_name);
        goto end_of_x_get_string_property;
    }

    if (xtp.nitems == 0) {
        debug("No items in string property '%s'", prop_name);
        goto end_of_x_get_string_property;
    }

    if (xtp.encoding == XA_STRING) {
        out = x_copy_string_property_value(prop_name, xtp.value);
    }
    else {
        int nstrings = 0;
        int result   = XmbTextPropertyToTextList(ctx->dpy, &xtp,
                                                 &strings, &nstrings);
        if (result >= Success && nstrings > 0 && strings && strings[0]) {
            out = x_copy_string_property_value(prop_name, strings[0]);
        }
    }

end_of_x_get_string_property:
    if (strings) {
        XFreeStringList(strings);
    }
    if (xtp.value) {
        XFree(xtp.value);
    }

    if (out) {
        debug("Got '%s' value: '%s'", prop_name, out);
    }
    else {
        debug("No value for '%s' property", prop_name);
    }

    return out;
}

static char *x_get_title(Context *ctx, Window window)
{
    char *title = x_get_string_property(ctx, window, "_NET_WM_NAME");
    if (!title) {
        title = x_get_string_property(ctx, window, "WM_NAME");
    }
    return title;
}

static int x_snap_window(Context *ctx, Window window,
                         Window parent, int depth);

static int x_snap_children(Context *ctx, Window parent, int depth,
                           Window *children, unsigned int nchildren)
{
    int focused = 0;
    for (unsigned int i = 0; i < nchildren; ++i) {
        int child_focused = x_snap_window(ctx, children[i], parent, depth);
        if (child_focused != 0) {
            focused = child_focused;
        }
    }
    return focused;
}

static int x_get_children(Context *ctx, Window window, int depth)
{
    Window       root, parent, *children;
    unsigned int nchildren;
    if (XQueryTree(ctx->dpy, window, &root, &parent,
                   &children, &nchildren) >= Success) {
        if (children) {
            int focused =
                x_snap_children(ctx, window, depth, children, nchildren);
            XFree(children);
            return focused;
        }
    }
    else {
        debug("Can't get children of window %llu", (unsigned long long) window);
    }
    return 0;
}

static int x_snap_window(Context *ctx, Window window, Window parent, int depth)
{
    debug("Capturing snapshot of window %llu", (unsigned long long) window);
    bool have_property = false;
    int  child_focused = x_get_children(ctx, window, depth + 1);
    int  focused       = child_focused != 0   ? child_focused
                       : ctx->focus == window ? depth
                       : 0;

    db_reset_stmt(ctx);
    db_bind_int(ctx, 1, ctx->snapshot_id);
    db_bind_unsigned_long_long_as_text(ctx, 2, window);
    if (parent != None) {
        db_bind_unsigned_long_long_as_text(ctx, 3, parent);
    }
    db_bind_int(ctx, 4, depth);
    db_bind_int(ctx, 5, focused);

    XClassHint *ch = ctx->ch;
    if (XGetClassHint(ctx->dpy, window, ch) >= Success) {
        if (ch->res_name) {
            have_property = have_property || strlen(ch->res_name) > 0;
            db_bind_string(ctx, 6, ch->res_name);
            XFree(ch->res_name);
            ch->res_name = NULL;
        }
        if (ch->res_class) {
            have_property = have_property || strlen(ch->res_class) > 0;
            db_bind_string(ctx, 7, ch->res_class);
            XFree(ch->res_class);
            ch->res_class = NULL;
        }
    }
    else {
        debug("No class hint for window %llu", (unsigned long long) window);
    }

    char *title = x_get_title(ctx, window);
    if (title) {
        have_property = have_property || strlen(title) > 0;
        db_bind_string(ctx, 8, title);
        free(title);
    }
    else {
        debug("No title for window %llu", (unsigned long long) window);
    }

    /*
     * If there's neither a name nor a class nor a title, you can't actually
     * classify anything about this window. Exclude it if so instructed.
     */
    if (!ctx->exclude_blanks || have_property) {
        db_exec_stmt(ctx, NULL);
    }
    else {
        debug("Not inserting empty entry for window %llu",
              (unsigned long long) window);
    }

    return focused;
}

static void x_recurse_windows(Context *ctx)
{
    db_prepare_window_insert(ctx);
    x_snap_window(ctx, ctx->root, None, 1);
    ctx->stmt = db_close_stmt(ctx->stmt);
}


static void run(Context *ctx)
{
    db_check_name(ctx);
    db_open(ctx);
    db_init(ctx);
    x_open_display(ctx);
    x_get_idle_time(ctx);
    db_begin(ctx);
    db_insert_snapshot(ctx);
    x_get_focused_window(ctx);
    x_alloc_class_hint(ctx);
    x_recurse_windows(ctx);
    db_commit(ctx);
}

static void cleanup(Context *ctx)
{
    debug("Cleaning up");
    ctx->stmt = db_close_stmt(ctx->stmt);
    ctx->tx   = db_rollback(ctx->db, ctx->tx);
    ctx->ch   = x_free_class_hint(ctx->ch);
    ctx->dpy  = x_close_display(ctx->dpy);
    ctx->db   = db_close(ctx->db);
    if (ctx->free_db_name) {
        free((char *)ctx->db_name);
    }
}


static int args_handle(Context *ctx, const char *prog, int opt)
{
    switch (opt) {
        case 'b':
            ctx->exclude_blanks = false;
            debug("exclude_blanks set to false");
            return 0;
        case 'B':
            ctx->exclude_blanks = true;
            debug("exclude_blanks set to true");
            return 0;
        case 'd':
            ctx->dpy_name = optarg;
            debug("dpy_name set to '%s'", ctx->dpy_name);
            return 0;
        case 'f':
            ctx->db_name = optarg;
            debug("db_name set to '%s'", ctx->db_name);
            return 0;
        case 'h':
            return ARGS_WANT_HELP;
        case 's':
            ctx->sample_time = atoi(optarg);
            debug("sample_time set to %d from '%s'", ctx->sample_time, optarg);
            if (ctx->sample_time > 0) {
                return 0;
            }
            else {
                warn("%s: invalid argument to -s -- '%s'", prog, optarg);
                return ARGS_ERROR;
            }
        default:
            return ARGS_ERROR;
    }
}

static int args_parse(Context *ctx, int argc, char **argv)
{
    int opt;
    int ret = 0;

    while ((opt = getopt(argc, argv, "bBd:f:hs:")) != -1) {
        ret |= args_handle(ctx, argv[0], opt);
    }

    if (optind != argc) {
        fprintf(stderr, "%s: trailing arguments --", argv[0]);
        for (int i = optind; i < argc; ++i) {
            fprintf(stderr, " %s", argv[i]);
        }
        fputs("\n", stderr);
        ret |= ARGS_ERROR;
    }

    if (ret & ARGS_WANT_HELP) {
        fprintf(stdout, args_help, argv[0]);
    }

    return ret;
}

int main(int argc, char **argv)
{
    XSetErrorHandler(x_handle_error);

    Context ctx     = {0};
    ctx.db_name     = NULL;
    ctx.dpy_name    = "";
    ctx.sample_time = 60;

    int arg_ret = args_parse(&ctx, argc, argv);
    if (arg_ret & ARGS_ERROR) {
        return 2;
    }
    else if (arg_ret & ARGS_WANT_HELP) {
        return 0;
    }

    int result = setjmp(ctx.env);
    if (result == 0) {
        debug("Running with longjmp buffer");
        run(&ctx);
    }
    else {
        debug("Caught longjmp %d", result);
    }

    cleanup(&ctx);
    return result;
}
