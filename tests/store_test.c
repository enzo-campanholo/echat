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

static void build(Store *s, unsigned int l, unsigned long i, const char *text)
{
    Ref r;
    r.l = l;
    r.i = i;
    assert(store_build(s, r, text));
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
    assert(store_open(&s, dir));
    assert(fcntl(s.lockfd, F_GETFD) & FD_CLOEXEC);
    assert(!store_open(&second, dir));
    assert(strstr(second.error, "writer"));
    store_close(&second);
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        status = store_open(&second, dir);
        store_close(&second);
        _exit(status ? 1 : 0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    assert(store_append(&s, "user", "one\nline", &id, &count));
    assert(id == 0 && count == 1);
    assert(store_append(&s, "unii", "two", NULL, NULL));
    assert(store_append(&s, "note", "three", NULL, NULL));
    assert(store_append(&s, "work", "[Worker] four", NULL, NULL));
    build(&s, 0, 0, "user: one\nline");
    build(&s, 0, 1, "unii: two");
    build(&s, 0, 2, "note: three");
    build(&s, 0, 3, "work: [Worker] four");
    build(&s, 1, 0, "one and two");
    build(&s, 1, 1, "three and four");
    build(&s, 2, 0, "all four");
    r.l = 2;
    r.i = 0;
    assert(!store_build(&s, r, "duplicate"));
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
    assert(store_save_view(&s));
    date = xstrdup(store_date(&s, 0));
    store_close(&s);
    /* Preserve an unusual live view, including whitespace, byte for byte. */
    put_file(dir, "view.json", " [ [1,0],\n[0,2], [0,3] ] \n");
    saved = get_file(dir, "view.json");
    assert(store_open(&s, dir));
    assert(s.view.n == 3 && s.view.v[0].l == 1);
    assert(s.view.shrinking && s.generation == 7);
    after = get_file(dir, "view.json");
    assert(!strcmp(saved, after));
    assert(!strcmp(store_date(&s, 0), date));
    zoom = store_zoom(&s, 0, 1, 0);
    assert(zoom && strstr(zoom, "kind=user") && strstr(zoom, "one\nline"));
    free(zoom);
    zoom = store_zoom(&s, 0, 2, 0);
    assert(zoom && !strcmp(zoom, "0+1|user: one line\n1+1|unii: two\n"));
    free(zoom);
    assert(!store_zoom(&s, 1, 2, 0));
    assert(!store_zoom(&s, 0, 3, 0));
    assert(!store_zoom(&s, 0, 1, 1));
    assert(!store_date(&s, 4));
    path = xstrdup(s.messages[0].path);
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
    assert(store_open(&s, dir));
    buf_init(&source);
    buf_init(&joined);
    buf_init(&echo);
    for (k = 0; k < 29999; k++)
        buf_putc(&source, 'a');
    buf_puts(&source, "\xe2\x82\xac");
    for (k = 0; k < 29998; k++)
        buf_putc(&source, 'b');
    assert(store_append(&s, "user", source.s, &first, &count));
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
    clipped = clip_echo(echo.s);
    assert(store_append(&s, "echo", echo.s, &first, &count));
    assert(first == 3 && count == 1);
    message = store_message(&s, first);
    assert(message && !strcmp(message, clipped) && !strstr(message, "DROP_THIS"));
    free(message);
    buf_reset(&joined);
    for (page = 0; ; page++) {
        zoom = store_zoom(&s, first, 1, page);
        if (!zoom)
            break;
        body = strchr(zoom, '\n');
        assert(body && strlen(body + 1) <= MESSAGE_BYTES && utf8_valid(body + 1));
        buf_puts(&joined, body + 1);
        free(zoom);
    }
    assert(page > 1 && !strcmp(joined.s, clipped));
    free(clipped);
    assert(store_append(&s, "note", "", &first, &count));
    assert(count == 1);
    assert(!store_append(&s, "thought", "not logged", NULL, NULL));
    assert(!store_append(&s, "user", "\xc0\x80", NULL, NULL));
    store_close(&s);
    assert(store_open(&s, dir));
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

static void crash_tail_and_state(void)
{
    Store s;
    char *dir, *saved, *after;
    dir = new_dir();
    assert(store_open(&s, dir));
    assert(store_append(&s, "user", "first", NULL, NULL));
    assert(store_append(&s, "unii", "second", NULL, NULL));
    store_close(&s);
    put_file(dir, "view.json", "[[0,0]]\n");
    assert(store_open(&s, dir));
    assert(s.view.n == 2 && s.view.v[1].i == 1);
    store_close(&s);
    /* A crash after metadata but before saving the view uses prior state. */
    put_file(dir, ".state", "{\"generation\":8,\"shrinking\":0,"
             "\"previous_shrinking\":1,\"view\":[[0,0]]}\n");
    saved = get_file(dir, "view.json");
    assert(store_open(&s, dir));
    assert(s.view.shrinking == 1 && s.generation == 8);
    after = get_file(dir, "view.json");
    assert(!strcmp(saved, after));
    free(saved);
    free(after);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

static void rejected_log(const char *main, const char *tree, const char *view)
{
    Store s;
    char *dir, *before, *after;
    dir = new_dir();
    assert(store_open(&s, dir));
    store_close(&s);
    put_file(dir, "main/2026-10-08.jsonl", main);
    put_file(dir, "tree/2026-10-08.jsonl", tree);
    put_file(dir, "view.json", view);
    before = get_file(dir, "main/2026-10-08.jsonl");
    assert(!store_open(&s, dir));
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
    rejected_log("{", "", "[]");
    rejected_log("{}\n", "", "[]");
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
    assert(store_open(&s, dir));
    assert(store_append(&s, "user", "preserved", NULL, NULL));
    store_close(&s);
    path = path_join(dir, "view.json");
    assert(!unlink(path));
    free(path);
    assert(!store_open(&s, dir));
    assert(strstr(s.error, "missing"));
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

int main(void)
{
    restart_and_lock();
    split_and_pages();
    crash_tail_and_state();
    corrupt_files();
    puts("store: ok");
    return 0;
}
