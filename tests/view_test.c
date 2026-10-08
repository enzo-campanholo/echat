#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void build(Store *s, unsigned long id, const char *text)
{
    Ref r, sibling;
    Node *n;
    r.l = 0;
    r.i = id;
    for (;;) {
        n = store_slot(s, r);
        n->text = xstrdup(text);
        n->size = strlen(text);
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

static void clear(Store *s)
{
    size_t i;
    unsigned int l;
    for (l = 0; l < TREE_LEVELS; l++) {
        for (i = 0; i < s->capacities[l]; i++)
            free(s->levels[l][i].text);
        free(s->levels[l]);
    }
    view_free(&s->view);
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
    clear(&s);
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
            now = view_render(&s, &s.view, s.count, 0);
            /* Closing tags move; all previous summary bytes remain a prefix. */
            assert(!strncmp(old, now, strlen(old) - (strlen(old) >= 8 ? 8 : 0)));
            free(now);
        }
        free(old);
        old = view_render(&s, &s.view, s.count, 0);
    }
    assert(batches > 10);
    free(old);
    clear(&s);
}

static void availability(void)
{
    Store s;
    Ref a, b, p;
    Node *n;
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
    n = store_slot(&s, p);
    n->text = xstrdup("user: short");
    n->size = strlen(n->text);
    assert(view_shrink(&s, &s.view, 128000, 64000, 0) == 0);
    assert(!s.view.shrinking); /* already below low budget */
    assert(view_shrink(&s, &s.view, 0, 0, 1));
    assert(s.view.n == 1 && s.view.v[0].l == 1);
    clear(&s);
}

int main(void)
{
    equivalence();
    sawtooth();
    availability();
    puts("view: rollback equivalence through 20,000 pushes; byte sawtooth; availability OK");
    return 0;
}
