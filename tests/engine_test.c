#define _POSIX_C_SOURCE 200809L
#include "../src/engine.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *new_dir(void)
{
    char *p;
    p = xstrdup("/tmp/uniichat-engine-XXXXXX");
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
        assert(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode))
            remove_dir(p);
        else
            assert(unlink(p) == 0);
        free(p);
    }
    assert(closedir(d) == 0);
    assert(rmdir(path) == 0);
}

static char *repeated(size_t n, int c)
{
    char *p;
    p = xmalloc(n + 1);
    memset(p, c, n);
    p[n] = 0;
    return p;
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

static void put_file(const char *dir, const char *name, const char *text)
{
    char *p;
    FILE *f;
    p = path_join(dir, name);
    f = fopen(p, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
    free(p);
}

static Ref ref(unsigned int l, unsigned long i)
{
    Ref r;
    r.l = l;
    r.i = i;
    return r;
}

static void check_job(Job j, unsigned int l, unsigned long i)
{
    assert(j.ref.l == l && j.ref.i == i);
}

static void ruler(const char *task)
{
    const char *p;
    size_t i;
    p = strstr(task, "the length of this ruler:\n");
    assert(p);
    p += strlen("the length of this ruler:\n");
    for (i = 0; i < NODE_BYTES; i++)
        assert(p[i] == '-');
    assert(p[NODE_BYTES] == '\n');
    assert(p[NODE_BYTES + 1] != '-');
}

static void shortcuts_and_tasks(void)
{
    Store s;
    Engine e;
    Job j;
    char *dir, *short_text, *long_text, *task;
    Node *n;
    dir = new_dir();
    assert(store_open(&s, dir));
    short_text = repeated(506, 'a');
    long_text = repeated(507, 'b');
    assert(store_append(&s, "user", short_text, NULL, NULL));
    assert(store_append(&s, "unii", long_text, NULL, NULL));
    assert(store_append(&s, "note", "small", NULL, NULL));
    assert(engine_open(&e, &s) == 0);
    assert(engine_next(&e, &j) == 1);
    check_job(j, 0, 1);
    n = store_node(&s, ref(0, 0));
    assert(n && n->size == NODE_BYTES);
    assert(!memcmp(n->text, "user: ", 6));
    assert(!strcmp(n->text + 6, short_text));
    assert(!store_node(&s, ref(1, 0)));
    task = engine_task(&e, j);
    assert(task && strstr(task, "Compaction: compress message 1 "));
    assert(strstr(task, long_text));
    ruler(task);
    free(task);
    assert(engine_done(&e, j, "unii: compressed") == 0);
    assert(engine_next(&e, &j) == 1);
    check_job(j, 1, 0);
    task = engine_task(&e, j);
    assert(task && strstr(task, "Compaction: merge lines 0+1 and 1+1"));
    assert(strstr(task, "<chat> may hold their messages, 0 to 1"));
    assert(strstr(task, short_text) && strstr(task, "unii: compressed"));
    ruler(task);
    free(task);
    assert(engine_done(&e, j, "user requested a; unii replied b") == 0);
    assert(engine_next(&e, &j) == 0);
    n = store_node(&s, ref(0, 2));
    assert(n && !strcmp(n->text, "note: small"));
    assert(engine_caught_up(&e, 3));
    assert(!engine_caught_up(&e, 4));
    engine_close(&e);
    store_close(&s);
    free(short_text);
    free(long_text);
    remove_dir(dir);
    free(dir);
}

/* All leaves and their joins fit: no model job is needed at any level. */
static void sibling_shortcuts_once(void)
{
    Store s;
    Engine e;
    Job j;
    char *dir, *tree_before, *tree_after, *path;
    Ref r;
    unsigned int l;
    unsigned long i;
    Node *n;
    dir = new_dir();
    assert(store_open(&s, dir));
    for (i = 0; i < 16; i++)
        assert(store_append(&s, "user", "x", NULL, NULL));
    assert(engine_open(&e, &s) == 0);
    assert(engine_next(&e, &j) == 0);
    assert(e.inflight == 0 && e.contiguous == 16);
    for (l = 0; l <= 4; l++) {
        for (i = 0; i < (16UL >> l); i++) {
            r = ref(l, i);
            n = store_node(&s, r);
            assert(n && n->size == ref_span(r) * 8 - 1);
            assert(!n->queued && !n->busy);
        }
    }
    n = store_node(&s, ref(1, 0));
    assert(!strcmp(n->text, "user: x\nuser: x"));
    path = path_join(dir, "tree");
    {
        DIR *d;
        struct dirent *entry;
        char *file;
        tree_before = NULL;
        d = opendir(path);
        assert(d);
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            file = path_join(path, entry->d_name);
            assert(!tree_before);
            tree_before = read_file(file);
            assert(tree_before);
            free(file);
        }
        assert(closedir(d) == 0 && tree_before);
    }
    engine_close(&e);
    store_close(&s);
    assert(store_open(&s, dir));
    assert(engine_open(&e, &s) == 0);
    assert(engine_next(&e, &j) == 0);
    assert(e.contiguous == 16);
    {
        DIR *d;
        struct dirent *entry;
        char *file;
        tree_after = NULL;
        d = opendir(path);
        assert(d);
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            file = path_join(path, entry->d_name);
            assert(!tree_after);
            tree_after = read_file(file);
            assert(tree_after);
            free(file);
        }
        assert(closedir(d) == 0 && tree_after);
    }
    assert(!strcmp(tree_before, tree_after));
    free(tree_before);
    free(tree_after);
    free(path);
    engine_close(&e);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

static void predecessor_window_and_retry(void)
{
    Store s;
    Engine e;
    Job jobs[16], j, retry;
    char *dir, *long_text, *context;
    unsigned long i;
    size_t k;
    int rc;
    dir = new_dir();
    assert(store_open(&s, dir));
    long_text = repeated(600, 'q');
    for (i = 0; i < 16; i++)
        assert(store_append(&s, "user", long_text, NULL, NULL));
    assert(engine_open(&e, &s) == 0);
    for (k = 0; k < WORKERS; k++) {
        assert(engine_next(&e, &jobs[k]) == 1);
        check_job(jobs[k], 0, (unsigned long)k);
    }
    assert(e.inflight == WORKERS);
    assert(engine_next(&e, &j) == 0);
    engine_fail(&e, jobs[0]);
    assert(e.inflight == WORKERS);
    assert(engine_next(&e, &j) == 0);
    for (k = 1; k < WORKERS; k++)
        assert(engine_done(&e, jobs[k], "user: summarized") == 0);
    assert(e.inflight == 1 && !e.contiguous);
    for (k = WORKERS; k < WORKERS * 2 - 1; k++) {
        assert(engine_next(&e, &jobs[k]) == 1);
        check_job(jobs[k], 0, (unsigned long)k);
    }
    assert(e.inflight == WORKERS);
    assert(engine_next(&e, &j) == 0);
    assert(!store_node(&s, ref(0, 15)));
    context = engine_context(&e, jobs[14]);
    assert(context && !strcmp(context, "<chat>\n</chat>\n"));
    free(context);
    /* Synchronizing without a new message must not retry failed work. */
    assert(engine_sync(&e) == 0);
    assert(engine_next(&e, &j) == 0);
    assert(store_append(&s, "note", "immediate new message", NULL, NULL));
    assert(engine_sync(&e) == 0);
    assert(engine_next(&e, &retry) == 1);
    check_job(retry, 0, 0);
    assert(retry.retry && e.inflight == WORKERS);
    assert(engine_done(&e, retry, "user: recovered") == 0);
    assert(e.inflight == WORKERS - 1 && e.contiguous == WORKERS);
    assert(engine_next(&e, &jobs[15]) == 1);
    check_job(jobs[15], 0, 15);
    assert(e.inflight == WORKERS);
    assert(engine_next(&e, &j) == 0);
    assert(!store_node(&s, ref(0, 16)));
    for (k = WORKERS; k < 16; k++)
        assert(engine_done(&e, jobs[k], "user: summarized") == 0);
    do {
        rc = engine_next(&e, &j);
        assert(rc >= 0);
        if (rc == 1)
            assert(engine_done(&e, j, "merged") == 0);
    } while (rc == 1);
    assert(e.contiguous == 17 && e.inflight == 0);
    assert(engine_caught_up(&e, 17));
    assert(!strcmp(store_node(&s, ref(0, 16))->text,
                   "note: immediate new message"));
    engine_close(&e);
    store_close(&s);
    free(long_text);
    remove_dir(dir);
    free(dir);
}

/* Complete siblings out of order and keep several parent calls in flight. */
static void parent_jobs_once(void)
{
    Store s;
    Engine e;
    Job jobs[8], j;
    char *dir, *message, *summary;
    unsigned int seen[4][8];
    size_t n, k, total;
    unsigned long i;
    int rc;
    dir = new_dir();
    message = repeated(600, 'm');
    summary = repeated(400, 's');
    memset(seen, 0, sizeof(seen));
    assert(store_open(&s, dir));
    for (i = 0; i < 8; i++)
        assert(store_append(&s, "user", message, NULL, NULL));
    assert(engine_open(&e, &s) == 0);
    total = 0;
    for (;;) {
        n = 0;
        while ((rc = engine_next(&e, &j)) == 1) {
            assert(n < 8 && j.ref.l < 4 && j.ref.i < 8);
            assert(!seen[j.ref.l][j.ref.i]);
            seen[j.ref.l][j.ref.i] = 1;
            jobs[n++] = j;
            total++;
            if (j.ref.l) {
                assert(store_node(&s, ref(j.ref.l - 1, j.ref.i * 2)));
                assert(store_node(&s, ref(j.ref.l - 1, j.ref.i * 2 + 1)));
            }
        }
        assert(rc == 0);
        if (!n)
            break;
        for (k = n; k; k--)
            assert(engine_done(&e, jobs[k - 1], summary) == 0);
    }
    assert(total == 15 && e.inflight == 0 && e.contiguous == 8);
    assert(store_node(&s, ref(3, 0)));
    engine_close(&e);
    store_close(&s);
    assert(store_open(&s, dir));
    assert(engine_open(&e, &s) == 0);
    assert(engine_next(&e, &j) == 0);
    engine_close(&e);
    store_close(&s);
    free(message);
    free(summary);
    remove_dir(dir);
    free(dir);
}

static void context_boundaries_and_persistence(void)
{
    Store s;
    Engine e;
    Job j;
    View original;
    char *dir, *context, *saved, *after;
    unsigned long i;
    size_t k;
    dir = new_dir();
    assert(store_open(&s, dir));
    for (i = 0; i < 8; i++)
        assert(store_append(&s, "user", "x", NULL, NULL));
    assert(engine_open(&e, &s) == 0);
    assert(engine_next(&e, &j) == 0);
    view_init(&original);
    view_copy(&original, &s.view);
    /* Use a deliberately coarse persistent prefix to test boundary splitting. */
    put_file(dir, "compaction-view.json",
             " {\"shrinking\":0, \"view\":[[2,0], [2,1]]} \n");
    saved = get_file(dir, "compaction-view.json");
    engine_close(&e);
    assert(engine_open(&e, &s) == 0);
    assert(e.compact.n == 2 && e.compact.v[0].l == 2);
    assert(s.view.n == original.n);
    for (k = 0; k < original.n; k++)
        assert(s.view.v[k].l == original.v[k].l &&
               s.view.v[k].i == original.v[k].i);
    after = get_file(dir, "compaction-view.json");
    assert(!strcmp(saved, after));
    free(after);
    j.ref = ref(0, 3);
    j.retry = 0;
    context = engine_context(&e, j);
    assert(context && !strcmp(context,
           "<chat>\n0+2|user: x user: x\n2+1|user: x\n</chat>\n"));
    free(context);
    j.ref = ref(1, 1);
    context = engine_context(&e, j);
    assert(context && !strcmp(context,
           "<chat>\n0+4|user: x user: x user: x user: x\n</chat>\n"));
    free(context);
    /* Message 8 is missing; message 9 built out of order must not appear. */
    assert(store_append(&s, "note", "missing", NULL, NULL));
    assert(store_append(&s, "note", "later", NULL, NULL));
    assert(store_append(&s, "note", "target", NULL, NULL));
    assert(engine_sync(&e) == 0);
    assert(store_build(&s, ref(0, 9), "note: later"));
    j.ref = ref(0, 10);
    context = engine_context(&e, j);
    assert(context && !strstr(context, "missing") && !strstr(context, "later"));
    assert(!strstr(context, "not summarized"));
    assert(strstr(context, "4+4|"));
    free(context);
    engine_close(&e);
    store_close(&s);
    assert(store_open(&s, dir));
    assert(engine_open(&e, &s) == 0);
    assert(e.compact.n == 5 && e.compact.v[0].l == 2 &&
           e.compact.v[2].i == 8 && e.compact.v[3].i == 9 &&
           e.compact.v[4].i == 10);
    assert(engine_next(&e, &j) == 0);
    assert(e.contiguous == 11);
    engine_close(&e);
    store_close(&s);
    free(saved);
    view_free(&original);
    remove_dir(dir);
    free(dir);
}

static void rejected_compaction_view(void)
{
    Store s;
    Engine e;
    char *dir, *saved, *after;
    dir = new_dir();
    assert(store_open(&s, dir));
    assert(store_append(&s, "user", "x", NULL, NULL));
    put_file(dir, "compaction-view.json", "{\"shrinking\":0,\"view\":[[0,1]]}\n");
    saved = get_file(dir, "compaction-view.json");
    assert(engine_open(&e, &s) == -1);
    assert(strstr(s.error, "refusing to rebuild"));
    after = get_file(dir, "compaction-view.json");
    assert(!strcmp(saved, after));
    free(saved);
    free(after);
    engine_close(&e);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

static void unbuilt_compaction_parent(void)
{
    Store s;
    Engine e;
    char *dir, *saved, *after, summary[501];
    unsigned long i;
    dir = new_dir();
    assert(store_open(&s, dir));
    memset(summary, 's', sizeof(summary) - 1);
    summary[sizeof(summary) - 1] = 0;
    for (i = 0; i < 4; i++) {
        assert(store_append(&s, "note", "source", NULL, NULL));
        assert(store_build(&s, ref(0, i), summary));
    }
    put_file(dir, "compaction-view.json", "{\"shrinking\":0,\"view\":[[2,0]]}\n");
    saved = get_file(dir, "compaction-view.json");
    assert(engine_open(&e, &s) == -1);
    assert(strstr(s.error, "refusing to rebuild"));
    after = get_file(dir, "compaction-view.json");
    assert(!strcmp(saved, after));
    free(saved); free(after);
    engine_close(&e);
    store_close(&s);
    remove_dir(dir);
    free(dir);
}

int main(void)
{
    shortcuts_and_tasks();
    sibling_shortcuts_once();
    predecessor_window_and_retry();
    parent_jobs_once();
    context_boundaries_and_persistence();
    rejected_compaction_view();
    unbuilt_compaction_parent();
    puts("engine: ok");
    return 0;
}
