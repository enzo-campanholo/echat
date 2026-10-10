#define _POSIX_C_SOURCE 200809L
#include "../src/store.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char *new_dir(void)
{
    char *p;
    p = xstrdup("/tmp/uniichat-store-XXXXXX");
    assert(mkdtemp(p));
    return p;
}

static void remove_dir(const char *path)
{
    DIR *d;
    struct dirent *entry;
    struct stat st;
    char *p;
    d = opendir(path);
    assert(d);
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        p = path_join(path, entry->d_name);
        assert(!lstat(p, &st));
        if (S_ISDIR(st.st_mode))
            remove_dir(p);
        else
            assert(!unlink(p));
        free(p);
    }
    assert(!closedir(d));
    assert(!rmdir(path));
}

static void put_file(const char *dir, const char *name, const char *text)
{
    char *p;
    FILE *f;
    p = path_join(dir, name);
    f = fopen(p, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
    free(p);
}

static char *get_file(const char *dir, const char *name)
{
    char *p, *text;
    p = path_join(dir, name);
    text = read_file(p);
    assert(text);
    free(p);
    return text;
}

/* Predicates over the 0/-1 store API keep the assertions short. */
static int opens(Store *s, const char *dir) { return !store_open(s, dir); }
static int reads(Store *s, const char *dir) { return !store_open_read(s, dir); }
static int saves(Store *s) { return !store_save_view(s); }
static int builds(Store *s, Ref r, const char *text) { return !store_build(s, r, text); }

static int logs(Store *s, const char *dir, unsigned long from, unsigned long count,
                FILE *out)
{
    return !store_log(s, dir, from, count, out);
}

static int appends(Store *s, const char *kind, const char *text, const Json *images,
                   unsigned long *first, unsigned long *count)
{
    Entry e;
    e.kind = kind;
    e.text = text;
    e.images = images;
    return !store_append(s, &e, 1, first, count);
}

static int dated(Store *s, unsigned long i, const char *prefix)
{
    char *date;
    int same;
    date = store_date(s, i);
    assert(date);
    same = !strncmp(date, prefix, strlen(prefix));
    free(date);
    return same;
}

static Ref ref(unsigned int l, unsigned long i)
{
    Ref r;
    r.l = l;
    r.i = i;
    return r;
}

static void build(Store *s, unsigned int l, unsigned long i, const char *text)
{
    Ref r;
    r.l = l;
    r.i = i;
    assert(builds(s, r, text));
}

static void restart_and_lock(void)
{
    Store s, second;
    char *dir, *saved, *after, *zoom, *date, *path;
    Ref r;
    pid_t pid;
    int status;
    unsigned long id, count;
    dir = new_dir();
    assert(opens(&s, dir));
    assert(fcntl(s.lockfd, F_GETFD) & FD_CLOEXEC);
    assert(!opens(&second, dir));
    assert(strstr(second.error, "writer"));
    store_close(&second);
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        status = opens(&second, dir);
        store_close(&second);
        _exit(status ? 1 : 0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    assert(appends(&s, "user", "one\nline", NULL, &id, &count));
    assert(id == 0 && count == 1);
    assert(appends(&s, "unii", "two", NULL, NULL, NULL));
    assert(appends(&s, "note", "three", NULL, NULL, NULL));
    assert(appends(&s, "work", "[Worker] four", NULL, NULL, NULL));
    build(&s, 0, 0, "user: one\nline");
    build(&s, 0, 1, "unii: two");
    build(&s, 0, 2, "note: three");
    build(&s, 0, 3, "work: [Worker] four");
    build(&s, 1, 0, "one and two");
    build(&s, 1, 1, "three and four");
    build(&s, 2, 0, "all four");
    r.l = 2;
    r.i = 0;
    assert(!builds(&s, r, "duplicate"));
    view_free(&s.view);
    r.l = 1;
    view_add(&s.view, r);
    r.l = 0;
    r.i = 2;
    view_add(&s.view, r);
    r.i = 3;
    view_add(&s.view, r);
    s.view.shrinking = 1;
    s.generation = 7;
    assert(saves(&s));
    date = store_date(&s, 0);
    store_close(&s);
    /* Preserve an unusual live view, including whitespace, byte for byte. */
    put_file(dir, "view.json", " [ [1,0],\n[0,2], [0,3] ] \n");
    saved = get_file(dir, "view.json");
    assert(opens(&s, dir));
    assert(s.view.n == 3 && s.view.v[0].l == 1);
    assert(s.view.shrinking && s.generation == 7);
    after = get_file(dir, "view.json");
    assert(!strcmp(saved, after));
    assert(dated(&s, 0, date));
    zoom = store_zoom(&s, 0, 1, 0, NULL);
    assert(zoom && strstr(zoom, "kind=user") && strstr(zoom, "one\nline"));
    free(zoom);
    zoom = store_zoom(&s, 0, 2, 0, NULL);
    assert(zoom && !strcmp(zoom, "0+1|user: one line\n1+1|unii: two\n"));
    free(zoom);
    assert(!store_zoom(&s, 1, 2, 0, NULL));
    assert(!store_zoom(&s, 0, 3, 0, NULL));
    assert(!store_zoom(&s, 0, 1, 1, NULL));
    assert(!store_date(&s, 4));
    path = xstrdup(s.files[s.messages[0].file]);
    store_close(&s);
    free(saved);
    free(after);
    free(date);
    free(path);
    remove_dir(dir);
    free(dir);
}

static void split_and_pages(void)
{
    Store s;
    Buf source, joined, echo;
    char *dir, *message, *zoom, *body, *clipped;
    unsigned long first, count, i, page;
    size_t k;
    dir = new_dir();
    assert(opens(&s, dir));
    buf_init(&source);
    buf_init(&joined);
    buf_init(&echo);
    for (k = 0; k < 29999; k++)
        buf_putc(&source, 'a');
    buf_puts(&source, "\xe2\x82\xac");
    for (k = 0; k < 29998; k++)
        buf_putc(&source, 'b');
    assert(appends(&s, "user", source.s, NULL, &first, &count));
    assert(first == 0 && count == 3);
    for (i = 0; i < count; i++) {
        message = store_message(&s, i);
        assert(message && strlen(message) <= MESSAGE_BYTES && utf8_valid(message));
        buf_puts(&joined, message);
        free(message);
    }
    assert(!strcmp(joined.s, source.s));
    for (k = 0; k < 15000; k++)
        buf_puts(&echo, "\xf0\x9f\x98\x80");
    buf_puts(&echo, "DROP_THIS");
    for (k = 0; k < 15000; k++)
        buf_puts(&echo, "\xc3\xa9");
    clipped = clip_echo(echo.s, ECHO_CHARS);
    assert(appends(&s, "echo", echo.s, NULL, &first, &count));
    assert(first == 3 && count == 1);
    message = store_message(&s, first);
    assert(message && !strcmp(message, clipped) && !strstr(message, "DROP_THIS"));
    free(message);
    buf_reset(&joined);
    for (page = 0; ; page++) {
        zoom = store_zoom(&s, first, 1, page, NULL);
        if (!zoom)
            break;
        body = strchr(zoom, '\n');
        assert(body && strlen(body + 1) <= MESSAGE_BYTES && utf8_valid(body + 1));
        buf_puts(&joined, body + 1);
        free(zoom);
    }
    assert(page > 1 && !strcmp(joined.s, clipped));
    free(clipped);
    assert(appends(&s, "note", "", NULL, &first, &count));
    assert(count == 1);
    assert(!appends(&s, "thought", "not logged", NULL, NULL, NULL));
    assert(!appends(&s, "user", "\xc0\x80", NULL, NULL, NULL));
    store_close(&s);
    assert(opens(&s, dir));
    assert(s.count == 5);
    for (i = 0; i < 3; i++) {
        message = store_message(&s, i);
        assert(message && utf8_valid(message));
        free(message);
    }
    store_close(&s);
    buf_free(&source);
    buf_free(&joined);
    buf_free(&echo);
    remove_dir(dir);
    free(dir);
}

static void durable_images(void)
{
    static const char *invalid[] = {
        "{}", "null", "[{}]", "[{\"type\":\"text\",\"data\":\"AA==\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"image\",\"data\":\"AA==\"}]",
        "[{\"type\":\"image\",\"data\":\"AA==\",\"mimeType\":\"text/plain\"}]",
        "[{\"type\":\"image\",\"data\":\"A===\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"image\",\"data\":\"A?==\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"image\",\"data\":\"AAA\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"image\",\"data\":\"\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"image\",\"data\":\"AA==\",\"mimeType\":\"image/png\",\"extra\":1}]",
        NULL
    };
    const char *image_json, *text;
    char *dir, *message, *images, *path, *before, *after;
    Json *j, *bad, *record;
    Store s;
    Buf long_text, joined, line;
    FILE *f;
    unsigned long first, count, i, size;
    size_t k;
    int rc;
    dir = new_dir();
    assert(opens(&s, dir));
    image_json = "[{\"type\":\"image\",\"data\":\"AA==\",\"mimeType\":\"image/png\"},"
                 "{\"type\":\"image\",\"data\":\"AQID\",\"mimeType\":\"image/jpeg\"}]";
    j = json_parse(image_json);
    assert(j);
    text = "before\n\xc3\xa9\"after";
    assert(appends(&s, "user", text, j, &first, &count));
    assert(first == 0 && count == 1 && s.messages[0].has_images);
    assert(s.messages[0].size == strlen(text));
    message = store_message(&s, 0);
    assert(message && !strcmp(message, text));
    free(message);
    images = store_images(&s, 0);
    assert(images && !strcmp(images, image_json));
    free(images);
    path = xstrdup(s.files[s.messages[0].file]);
    before = read_file(path);
    assert(before);
    for (k = 0; invalid[k]; k++) {
        bad = json_parse(invalid[k]);
        assert(bad);
        assert(!appends(&s, "user", "rejected", bad, NULL, NULL));
        assert(s.count == 1);
        json_free(bad);
    }
    after = read_file(path);
    assert(after && !strcmp(before, after));
    free(before);
    free(after);
    buf_init(&long_text);
    buf_init(&joined);
    buf_init(&line);
    for (k = 0; k < 50000; k++)
        buf_putc(&long_text, 'x');
    assert(appends(&s, "note", long_text.s, j, &first, &count));
    assert(first == 1 && count == 2);
    assert(s.messages[1].has_images && !s.messages[2].has_images);
    assert(store_images(&s, 2) == NULL);
    assert(appends(&s, "echo", "", j, &first, &count));
    assert(first == 3 && count == 1 && s.messages[3].has_images);
    assert(s.messages[3].size == 0);
    /* Every attachment is part of its complete, flushed message record. */
    f = fopen(path, "rb");
    assert(f);
    i = 0;
    while ((rc = read_line(f, &line)) == 1) {
        record = json_parse(line.s);
        assert(record);
        assert(json_ulong(json_get(record, "size"), &size));
        assert(size == s.messages[i].size);
        assert((json_get(record, "images") != NULL) == (i != 2));
        if (i == 0)
            assert(!strcmp(json_str(json_get(record, "text")), text));
        json_free(record);
        i++;
    }
    assert(rc == 0 && i == 4 && fclose(f) == 0);
    store_close(&s);
    /* Recover a view saved before the attachment-bearing crash tail. */
    put_file(dir, "view.json", "[[0,0]]\n");
    assert(opens(&s, dir));
    assert(s.count == 4 && s.view.n == 4);
    for (i = 0; i < 4; i++) {
        assert(s.messages[i].has_images == (i != 2));
        images = store_images(&s, i);
        if (i == 2)
            assert(!images);
        else
            assert(images && !strcmp(images, image_json));
        free(images);
    }
    message = store_message(&s, 0);
    assert(message && !strcmp(message, text));
    free(message);
    for (i = 1; i <= 2; i++) {
        message = store_message(&s, i);
        assert(message);
        buf_puts(&joined, message);
        free(message);
    }
    assert(!strcmp(joined.s, long_text.s));
    json_free(j);
    buf_free(&long_text);
    buf_free(&joined);
    buf_free(&line);
    free(path);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

static void crash_tail_and_state(void)
{
    Store s;
    char *dir, *saved, *after;
    dir = new_dir();
    assert(opens(&s, dir));
    assert(appends(&s, "user", "first", NULL, NULL, NULL));
    assert(appends(&s, "unii", "second", NULL, NULL, NULL));
    store_close(&s);
    put_file(dir, "view.json", "[[0,0]]\n");
    assert(opens(&s, dir));
    assert(s.view.n == 2 && s.view.v[1].i == 1);
    store_close(&s);
    /* A crash after metadata but before saving the view uses prior state:
     * .state names a view other than the one view.json holds. */
    put_file(dir, "view.json", "[[0,0],[0,1]]\n");
    put_file(dir, ".state", "{\"generation\":8,\"shrinking\":0,"
             "\"previous_shrinking\":1,\"view\":[[0,0]]}\n");
    saved = get_file(dir, "view.json");
    assert(opens(&s, dir));
    assert(s.view.shrinking == 1 && s.generation == 8);
    after = get_file(dir, "view.json");
    assert(!strcmp(saved, after));
    free(saved);
    free(after);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

static void add_bytes(const char *path, const char *bytes, size_t n)
{
    FILE *f;
    f = fopen(path, "ab");
    assert(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
}

static size_t file_size(const char *path)
{
    struct stat st;
    assert(!stat(path, &st));
    return (size_t)st.st_size;
}

/* One bad clock reading must not stop writes, nor reorder the log. */
static void clock_steps(void)
{
    Store s;
    char *dir;
    dir = new_dir();
    assert(opens(&s, dir));
    store_close(&s);
    put_file(dir, "main/2099-01-01.jsonl", "{\"i\":0,\"kind\":\"note\",\"text\":\"future\","
             "\"size\":6,\"date\":\"2099-01-01T00:00:00Z\"}\n");
    put_file(dir, "tree/2099-01-01.jsonl", "{\"l\":0,\"i\":0,\"text\":\"note: future\",\"size\":12}\n");
    put_file(dir, "view.json", "[[0,0]]\n");
    assert(opens(&s, dir));
    assert(appends(&s, "user", "now", NULL, NULL, NULL));
    assert(!strcmp(strrchr(s.files[s.messages[1].file], '/'), "/2099-01-01.jsonl"));
    assert(!dated(&s, 1, "2099")); /* the real clock */
    build(&s, 0, 1, "user: now");
    assert(!strcmp(s.tree_day, "2099-01-01.jsonl"));
    store_close(&s);
    assert(opens(&s, dir) && s.count == 2);
    assert(store_node(&s, ref(0, 1)));
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

/* An unacknowledged final record is set aside; earlier damage still stops. */
static void torn_tails(void)
{
    static const char partial[] = "{\"i\":1,\"kind\":\"user\",\"te";
    static const char zeros[] = "{\"l\":0,\0\0\0\0";
    Store s;
    Buf torn;
    char *dir, *main_path, *tree_path, *before, *after;
    dir = new_dir();
    assert(opens(&s, dir));
    assert(appends(&s, "user", "kept", NULL, NULL, NULL));
    build(&s, 0, 0, "user: kept");
    main_path = xstrdup(s.files[s.messages[0].file]);
    tree_path = path_join(dir, "tree");
    buf_init(&torn);
    buf_puts(&torn, tree_path);
    buf_putc(&torn, '/');
    buf_puts(&torn, s.tree_day);
    free(tree_path);
    tree_path = xstrdup(torn.s);
    store_close(&s);
    before = read_file(main_path);
    add_bytes(main_path, partial, sizeof(partial) - 1);
    add_bytes(tree_path, zeros, sizeof(zeros) - 1); /* a zero-filled tail */
    assert(opens(&s, dir));
    assert(s.count == 1 && store_node(&s, ref(0, 0)));
    after = read_file(main_path);
    assert(after && !strcmp(before, after));
    free(after);
    buf_reset(&torn);
    buf_puts(&torn, main_path);
    buf_puts(&torn, ".torn");
    after = read_file(torn.s);
    assert(after && !strcmp(after, partial));
    free(after);
    buf_reset(&torn);
    buf_puts(&torn, tree_path);
    buf_puts(&torn, ".torn");
    assert(file_size(torn.s) == sizeof(zeros) - 1);
    assert(appends(&s, "unii", "after repair", NULL, NULL, NULL));
    store_close(&s);
    assert(opens(&s, dir) && s.count == 2);
    store_close(&s);
    free(before);
    remove_dir(dir);
    free(dir);
    /* A partial line followed by more records, or in an earlier file, is
     * corruption that must be preserved, not trimmed. */
    dir = new_dir();
    assert(opens(&s, dir));
    store_close(&s);
    put_file(dir, "main/2026-10-07.jsonl", "{\"i\":0,\"kind\":\"user\",\"text\":\"x\","
             "\"size\":1,\"date\":\"2026-10-07T12:00:00Z\"}\n{\"i\":1,\"ki");
    put_file(dir, "main/2026-10-08.jsonl", "");
    put_file(dir, "view.json", "[[0,0]]\n");
    before = get_file(dir, "main/2026-10-07.jsonl");
    assert(!opens(&s, dir) && strstr(s.error, "partial"));
    store_close(&s);
    after = get_file(dir, "main/2026-10-07.jsonl");
    assert(!strcmp(before, after));
    free(before);
    free(after);
    remove_dir(dir);
    free(dir);
    free(main_path);
    free(tree_path);
    buf_free(&torn);
}

/* A crash can leave a temp file of an atomic save; the next writer removes
 * it, a reader never does (D10). */
static void stray_temps(void)
{
    Store s;
    char *dir, *a, *b;
    dir = new_dir();
    assert(opens(&s, dir));
    store_close(&s);
    put_file(dir, ".save-AbC123", "half a view");
    put_file(dir, ".compaction-view.tmp", "half a view");
    a = path_join(dir, ".save-AbC123");
    b = path_join(dir, ".compaction-view.tmp");
    assert(reads(&s, dir));
    store_close(&s);
    assert(!access(a, F_OK) && !access(b, F_OK));
    assert(opens(&s, dir));
    assert(access(a, F_OK) < 0 && access(b, F_OK) < 0);
    store_close(&s);
    free(a);
    free(b);
    remove_dir(dir);
    free(dir);
}

/* Zoom results are logged by reference: short, no copy, no images (C4). */
static void zoom_references(void)
{
    Store s;
    Buf long_text;
    char *dir, *zoom, *ref;
    size_t k;
    dir = new_dir();
    assert(opens(&s, dir));
    buf_init(&long_text);
    for (k = 0; k < 20000; k++) /* 40,000 bytes in one echo: two pages */
        buf_puts(&long_text, "\xc3\xa9");
    assert(appends(&s, "echo", long_text.s, NULL, NULL, NULL));
    assert(appends(&s, "user", "short", NULL, NULL, NULL));
    buf_reset(&long_text);
    for (k = 0; k < 400; k++)
        buf_putc(&long_text, k % 2 ? 'a' : '\n');
    build(&s, 0, 0, long_text.s);
    build(&s, 0, 1, "user: short");
    zoom = store_zoom(&s, 0, 1, 1, &ref);
    assert(zoom && ref && !strcmp(ref, "message 0 page 1/2, 10000 bytes; not copied; zoom again to read"));
    free(zoom); free(ref);
    zoom = store_zoom(&s, 0, 2, 0, &ref);
    assert(zoom && ref && !strncmp(ref, "lines 0+1| a a", 14));
    assert(strstr(ref, "...; 1+1|user: short; not copied; zoom again to read"));
    assert(strlen(ref) < NODE_BYTES && !strchr(ref, '\n'));
    free(zoom); free(ref);
    assert(!store_zoom(&s, 0, 4, 0, &ref) && !ref);
    store_close(&s);
    buf_free(&long_text);
    remove_dir(dir);
    free(dir);
}

static size_t log_lines(Store *s, const char *dir, unsigned long from,
                        unsigned long count, unsigned long *ids)
{
    FILE *out;
    Buf line;
    Json *j;
    size_t n;
    out = tmpfile();
    assert(out && logs(s, dir, from, count, out));
    store_close(s);
    rewind(out);
    buf_init(&line);
    n = 0;
    while (read_line(out, &line) == 1) {
        j = json_parse(line.s);
        assert(j && json_ulong(json_get(j, "i"), &ids[n]));
        json_free(j);
        n++;
    }
    buf_free(&line);
    assert(!fclose(out));
    return n;
}

/* Readers never create, lock or write, and see a consistent snapshot. */
static void readers(void)
{
    Store w, r;
    FILE *scratch;
    Buf path;
    char *dir, *missing, *view, *state, *main_log, *tree_log, *after;
    unsigned long k, ids[8];
    dir = new_dir();
    missing = path_join(dir, "typo");
    assert(!reads(&r, missing) && strstr(r.error, "no such chat"));
    store_close(&r);
    assert(access(missing, F_OK) < 0);
    assert(opens(&w, dir));
    for (k = 0; k < 5; k++)
        assert(appends(&w, "user", "message", NULL, NULL, NULL));
    build(&w, 0, 0, "user: message");
    build(&w, 0, 1, "user: message");
    build(&w, 1, 0, "two messages");
    main_log = xstrdup(w.files[w.messages[0].file]);
    buf_init(&path);
    buf_puts(&path, dir);
    buf_puts(&path, "/tree/");
    buf_puts(&path, w.tree_day);
    tree_log = path.s;
    view = get_file(dir, "view.json");
    state = get_file(dir, ".state");
    /* Beside the writer, which still holds the lock. */
    assert(reads(&r, dir));
    assert(r.count == 5 && store_node(&r, ref(1, 0)) && r.view.n == 5);
    assert(!appends(&r, "user", "refused", NULL, NULL, NULL));
    assert(!builds(&r, ref(0, 2), "refused") && strstr(r.error, "read-only"));
    store_close(&r);
    /* A line still being written is not there yet; a node over messages
     * the reader has not seen was built later, as was all that follows. */
    add_bytes(main_log, "{\"i\":5,\"ki", 10);
    add_bytes(tree_log, "{\"l\":0,\"i\":7,\"text\":\"later\",\"size\":5}\n", 39);
    add_bytes(tree_log, "{\"l\":0,\"i\":2,\"text\":\"later\",\"size\":5}\n", 39);
    assert(reads(&r, dir));
    assert(r.count == 5 && store_node(&r, ref(1, 0)) && !store_node(&r, ref(0, 2)));
    store_close(&r);
    after = get_file(dir, "view.json");
    assert(!strcmp(view, after));
    free(after);
    after = get_file(dir, ".state");
    assert(!strcmp(state, after));
    free(after);
    buf_init(&path);
    buf_puts(&path, main_log);
    buf_puts(&path, ".torn");
    assert(access(path.s, F_OK) < 0);
    buf_free(&path);
    assert(log_lines(&r, dir, 1, 2, ids) == 2 && ids[0] == 1 && ids[1] == 2);
    assert(log_lines(&r, dir, 3, ULONG_MAX, ids) == 2 && ids[0] == 3 && ids[1] == 4);
    assert(log_lines(&r, dir, 9, 1, ids) == 0);
    store_close(&w);
    remove_dir(dir);
    free(dir);
    free(missing);
    /* Pages cross daily files, located by each file's first record. */
    dir = new_dir();
    missing = path_join(dir, "main");
    assert(!mkdir(missing, 0700));
    put_file(dir, "main/2026-01-01.jsonl",
             "{\"i\":0,\"kind\":\"note\",\"text\":\"a\",\"size\":1,\"date\":\"2026-01-01T00:00:00Z\"}\n"
             "{\"i\":1,\"kind\":\"note\",\"text\":\"b\",\"size\":1,\"date\":\"2026-01-01T00:00:00Z\"}\n");
    put_file(dir, "main/2026-01-02.jsonl",
             "{\"i\":2,\"kind\":\"note\",\"text\":\"c\",\"size\":1,\"date\":\"2026-01-02T00:00:00Z\"}\n"
             "{\"i\":3,\"kind\":\"note\",\"text\":\"d\",\"size\":1,\"date\":\"2026-01-02T00:00:00Z\"}\n");
    assert(log_lines(&r, dir, 1, 2, ids) == 2 && ids[0] == 1 && ids[1] == 2);
    assert(log_lines(&r, dir, 3, 5, ids) == 1 && ids[0] == 3);
    assert(log_lines(&r, dir, 0, 9, ids) == 4);
    put_file(dir, "main/2026-01-02.jsonl",
             "{\"i\":3,\"kind\":\"note\",\"text\":\"d\",\"size\":1,\"date\":\"2026-01-02T00:00:00Z\"}\n");
    scratch = tmpfile();
    assert(scratch && !logs(&r, dir, 1, 2, scratch) && strstr(r.error, "out-of-order"));
    assert(!fclose(scratch));
    store_close(&r);
    remove_dir(dir);
    free(dir);
    free(missing);
    free(view);
    free(state);
    free(main_log);
    free(tree_log);
}

static void rejected_log(const char *main, const char *tree, const char *view)
{
    Store s;
    char *dir, *before, *after;
    dir = new_dir();
    assert(opens(&s, dir));
    store_close(&s);
    put_file(dir, "main/2026-10-08.jsonl", main);
    put_file(dir, "tree/2026-10-08.jsonl", tree);
    put_file(dir, "view.json", view);
    before = get_file(dir, "main/2026-10-08.jsonl");
    assert(!opens(&s, dir));
    assert(s.error[0]);
    store_close(&s);
    after = get_file(dir, "main/2026-10-08.jsonl");
    assert(!strcmp(before, after));
    free(before);
    free(after);
    remove_dir(dir);
    free(dir);
}

static void corrupt_files(void)
{
    const char *valid, *node;
    Buf twice;
    Store s;
    char *dir, *path;
    valid = "{\"i\":0,\"kind\":\"user\",\"text\":\"x\",\"size\":1,"
            "\"date\":\"2026-10-08T12:00:00Z\"}\n";
    node = "{\"l\":0,\"i\":0,\"text\":\"user: x\",\"size\":7}\n";
    rejected_log("{\n", "", "[]");
    rejected_log("{}\n", "", "[]");
    rejected_log("{\"i\":0,\"kind\":\"user\",\"text\":\"x\",\"size\":1,"
                 "\"date\":\"2026-10-09T12:00:00Z\"}\n", "", "[[0,0]]");
    rejected_log("{\"i\":1,\"kind\":\"user\",\"text\":\"x\",\"size\":1,"
                 "\"date\":\"2026-10-08T12:00:00Z\"}\n", "", "[]");
    rejected_log("{\"i\":0,\"kind\":\"thought\",\"text\":\"x\",\"size\":1,"
                 "\"date\":\"2026-10-08T12:00:00Z\"}\n", "", "[]");
    rejected_log("{\"i\":0,\"kind\":\"user\",\"text\":\"x\",\"size\":2,"
                 "\"date\":\"2026-10-08T12:00:00Z\"}\n", "", "[]");
    rejected_log("{\"i\":0,\"kind\":\"user\",\"text\":\"x\",\"size\":1,"
                 "\"date\":\"2026-02-30T12:00:00Z\"}\n", "", "[]");
    rejected_log(valid, "", "[[0,1]]");
    rejected_log(valid, "", "[[0,0],[0,0]]");
    rejected_log(valid, "", "[[63,0]]");
    rejected_log(valid, "", "{}");
    rejected_log(valid, "{\"l\":0,\"i\":1,\"text\":\"x\",\"size\":1}\n", "[]");
    buf_init(&twice);
    buf_puts(&twice, node);
    buf_puts(&twice, node);
    rejected_log(valid, twice.s, "[[0,0]]");
    buf_reset(&twice);
    buf_puts(&twice, valid);
    buf_puts(&twice, "{\"i\":1,\"kind\":\"unii\",\"text\":\"y\",\"size\":1,"
             "\"date\":\"2026-10-08T12:00:00Z\"}\n");
    rejected_log(twice.s, "{\"l\":1,\"i\":0,\"text\":\"xy\",\"size\":2}\n",
                 "[[0,0],[0,1]]");
    buf_free(&twice);
    dir = new_dir();
    assert(opens(&s, dir));
    assert(appends(&s, "user", "preserved", NULL, NULL, NULL));
    store_close(&s);
    path = path_join(dir, "view.json");
    assert(!unlink(path));
    free(path);
    assert(!opens(&s, dir));
    assert(strstr(s.error, "missing"));
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

int main(void)
{
    restart_and_lock();
    split_and_pages();
    durable_images();
    crash_tail_and_state();
    clock_steps();
    torn_tails();
    readers();
    zoom_references();
    stray_temps();
    corrupt_files();
    puts("store: ok");
    return 0;
}
