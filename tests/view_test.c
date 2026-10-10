#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Rollback { unsigned long start[64]; int keep[64]; size_t n; } Rollback;

/* The spec's recursive push, independently represented as an array. */
static void push(Rollback *r, unsigned long state, size_t at)
{
    unsigned long old;
    if (at == r->n) {
        r->start[at] = state;
        r->keep[at] = 0;
        r->n++;
    } else if (!r->keep[at]) {
        r->keep[at] = 1;
    } else {
        old = r->start[at];
        r->start[at] = state;
        r->keep[at] = 0;
        push(r, old, at + 1);
    }
}

static FILE *tree_log; /* scratch log holding the test nodes' records */
static char *tree_path;

/* A built node is a record in a log, read back on demand like the store's. */
static void put(Store *s, Ref r, const char *text)
{
    Buf b;
    Node *n;
    long at;
    assert(!fseek(tree_log, 0, SEEK_END) && (at = ftell(tree_log)) >= 0);
    buf_init(&b);
    buf_puts(&b, "{\"l\":");
    buf_ulong(&b, r.l);
    buf_puts(&b, ",\"i\":");
    buf_ulong(&b, r.i);
    buf_puts(&b, ",\"text\":");
    buf_json(&b, text);
    buf_puts(&b, ",\"size\":");
    buf_ulong(&b, (unsigned long)strlen(text));
    buf_puts(&b, "}\n");
    assert(fwrite(b.s, 1, b.len, tree_log) == b.len && !fflush(tree_log));
    buf_free(&b);
    n = store_slot(s, r);
    n->offset = (off_t)at;
    n->file = store_file(s, tree_path);
    n->size = (unsigned int)strlen(text);
}

static void build(Store *s, unsigned long id, const char *text)
{
    Ref r, sibling;
    r.l = 0;
    r.i = id;
    for (;;) {
        put(s, r, text);
        sibling = r;
        sibling.i ^= 1;
        if (!store_node(s, sibling))
            break;
        r.l++;
        r.i /= 2;
        if (!store_valid_ref(s, r))
            break;
    }
}
static void equivalence(void)
{
    Store s;
    Rollback rb;
    Ref leaf;
    unsigned long t, end, span;
    size_t k, back;
    memset(&s, 0, sizeof(s));
    memset(&rb, 0, sizeof(rb));
    view_init(&s.view);
    for (t = 0; t <= 20000; t++) {
        s.count = t + 1;
        leaf.l = 0;
        leaf.i = t;
        build(&s, t, "user: x");
        view_add(&s.view, leaf);
        push(&rb, t, 0);
        view_shrink(&s, &s.view, 0, 0, rb.n);
        assert(s.view.n == rb.n);
        end = 0;
        for (k = 0; k < s.view.n; k++) {
            back = rb.n - k - 1;
            span = (back ? rb.start[back - 1] : s.count) - rb.start[back];
            assert(ref_start(s.view.v[k]) == rb.start[back]);
            assert(ref_span(s.view.v[k]) == span);
            assert(ref_start(s.view.v[k]) == end);
            end += span;
        }
        assert(end == s.count);
    }
    store_close(&s);
}

static void sawtooth(void)
{
    Store s;
    Ref r;
    unsigned long t;
    size_t oldn, oldlen;
    char text[513], *old, *now;
    int batches;
    memset(&s, 0, sizeof(s));
    memset(text, 'x', 512);
    text[512] = 0;
    view_init(&s.view);
    old = xstrdup("<chat>\n");
    batches = 0;
    for (t = 0; t < 3000; t++) {
        oldn = s.view.n;
        s.count = t + 1;
        r.l = 0;
        r.i = t;
        build(&s, t, text);
        view_add(&s.view, r);
        oldlen = view_measure(&s, &s.view);
        if (view_shrink(&s, &s.view, VIEW_HIGH, VIEW_LOW, 0)) {
            assert(oldlen > VIEW_HIGH);
            assert(s.view.bytes <= VIEW_LOW);
            batches++;
        } else {
            assert(s.view.n == oldn + 1);
            now = view_render(&s, &s.view, s.count, 0, 0);
            /* Closing tags move; all previous summary bytes remain a prefix. */
            assert(!strncmp(old, now, strlen(old) - (strlen(old) >= 8 ? 8 : 0)));
            free(now);
        }
        free(old);
        old = view_render(&s, &s.view, s.count, 0, 0);
        assert(s.view.bytes == strlen(old));
    }
    assert(batches > 10);
    free(old);
    store_close(&s);
}

static void availability(void)
{
    Store s;
    Ref a, b, p;
    memset(&s, 0, sizeof(s));
    s.count = 2;
    a.l = b.l = 0;
    a.i = 0;
    b.i = 1;
    view_add(&s.view, a);
    view_add(&s.view, b);
    assert(!view_shrink(&s, &s.view, 1, 1, 0));
    assert(s.view.shrinking && s.view.n == 2);
    p.l = 1;
    p.i = 0;
    put(&s, p, "user: short");
    assert(view_shrink(&s, &s.view, 128000, 64000, 0) == 0);
    assert(!s.view.shrinking); /* already below low budget */
    assert(view_shrink(&s, &s.view, 0, 0, 1));
    assert(s.view.n == 1 && s.view.v[0].l == 1);
    store_close(&s);
}

static void cutoff(void)
{
    Store s;
    Ref r;
    char *text, *bounded, *p, *end;
    unsigned long i, cut, id, n, covered;
    memset(&s, 0, sizeof(s));
    s.count = 16;
    for (i = 0; i < s.count; i++)
        build(&s, i, "note: summary");
    r.l = 4; r.i = 0;
    view_add(&s.view, r);
    for (cut = 0; cut <= s.count; cut++) {
        text = view_render(&s, &s.view, cut, 1, 0);
        covered = 0;
        for (p = text + 7; *p != '<'; p = end + 1) {
            id = strtoul(p, &end, 10);
            assert(end != p && *end == '+');
            n = strtoul(end + 1, &end, 10);
            assert(*end == '|' && n && !(n & (n - 1)));
            assert(id == covered && !(id % n) && n <= cut - covered);
            covered += n;
            end = strchr(end, '\n');
            assert(end);
        }
        assert(covered == cut && !strcmp(p, "</chat>\n"));
        bounded = view_render(&s, &s.view, cut, 1, strlen(text));
        assert(bounded && !strcmp(text, bounded));
        free(bounded);
        assert(!view_render(&s, &s.view, cut, 1, strlen(text) - 1));
        free(text);
    }
    assert(s.view.n == 1 && s.view.v[0].l == 4);
    store_close(&s);
}

int main(void)
{
    char dir[] = "/tmp/uniichat-view-XXXXXX";
    assert(mkdtemp(dir));
    tree_path = path_join(dir, "tree.jsonl");
    tree_log = fopen(tree_path, "ab");
    assert(tree_log);
    equivalence();
    sawtooth();
    availability();
    cutoff();
    assert(!fclose(tree_log) && !unlink(tree_path) && !rmdir(dir));
    free(tree_path);
    puts("view: rollback equivalence through 20,000 pushes; byte sawtooth; availability OK");
    return 0;
}
