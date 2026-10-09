#include "store.h"
#include <stdlib.h>
#include <string.h>

void view_init(View *v)
{
    memset(v, 0, sizeof(*v));
}

void view_free(View *v)
{
    free(v->v);
    view_init(v);
}

void view_add(View *v, Ref r)
{
    if (v->n == v->cap) {
        if (v->cap > (size_t)-1 / sizeof(*v->v) / 2) {
            fputs("unii: view is too large\n", stderr);
            exit(1);
        }
        v->cap = v->cap ? v->cap * 2 : 32;
        if (v->cap > (size_t)-1 / sizeof(*v->v)) {
            fputs("unii: view is too large\n", stderr);
            exit(1);
        }
        v->v = xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = r;
}

void view_copy(View *to, const View *from)
{
    view_free(to);
    if (from->n) {
        to->v = xmalloc(from->n * sizeof(*from->v));
        memcpy(to->v, from->v, from->n * sizeof(*from->v));
    }
    to->n = to->cap = from->n;
    to->bytes = from->bytes;
    to->shrinking = from->shrinking;
}

static void line(Buf *b, Ref r, const Node *n)
{
    const char *p;
    buf_ulong(b, ref_start(r));
    buf_putc(b, '+');
    buf_ulong(b, ref_span(r));
    buf_putc(b, '|');
    p = n ? n->text : "(not summarized yet: zoom it)";
    for (; *p; p++)
        buf_putc(b, (*p == '\n' || *p == '\r') ? ' ' : *p);
    buf_putc(b, '\n');
}

/* Open only the node crossing the cutoff. Its children already exist; this
 * preserves the saved view's merges and never exposes a later message. */
static int prefix_line(Store *s, Buf *b, Ref r, unsigned long end, int built_only)
{
    Node *n;
    Ref child;
    unsigned long start;
    start = ref_start(r);
    if (start >= end)
        return 0;
    if (ref_span(r) > end - start) {
        child.l = r.l - 1;
        child.i = r.i * 2;
        if (!prefix_line(s, b, child, end, built_only))
            return 0;
        child.i++;
        return prefix_line(s, b, child, end, built_only);
    }
    n = store_node(s, r);
    if (!n && built_only)
        return 0;
    line(b, r, n);
    return 1;
}

char *view_render(Store *s, View *v, unsigned long end, int built_only)
{
    Buf b;
    size_t i;
    buf_init(&b);
    buf_puts(&b, "<chat>\n");
    for (i = 0; i < v->n; i++) {
        if (!prefix_line(s, &b, v->v[i], end, built_only))
            break;
    }
    buf_puts(&b, "</chat>\n");
    return b.s;
}

size_t view_measure(Store *s, View *v)
{
    size_t k, size, bytes;
    unsigned long id, n;
    Node *node;
    bytes = 15; /* <chat>\n and </chat>\n */
    for (k = 0; k < v->n; k++) {
        node = store_node(s, v->v[k]);
        size = node ? node->size : sizeof("(not summarized yet: zoom it)") - 1;
        size += 3; /* +, |, LF */
        id = ref_start(v->v[k]);
        n = ref_span(v->v[k]);
        do { size++; id /= 10; } while (id);
        do { size++; n /= 10; } while (n);
        if (size > (size_t)-1 - bytes) {
            fputs("unii: view size overflow\n", stderr);
            exit(1);
        }
        bytes += size;
    }
    v->bytes = bytes;
    return v->bytes;
}

/* Compare exact rational ages without floating point or cross-product overflow.
 * The pair ends at end-1; count is T, so numerator is T-end+1.
 * All denominators are powers of two. Equal ages retain the older pair. */
static int more_due(unsigned long count, Ref a, Ref b)
{
    unsigned long an, bn, ad, bd, aq, bq, ar, br;
    ad = ref_span(a);
    bd = ref_span(b);
    an = count - ref_start(a) - 2 * ad + 1;
    bn = count - ref_start(b) - 2 * bd + 1;
    aq = an / ad;
    bq = bn / bd;
    if (aq != bq)
        return aq > bq;
    ar = an % ad;
    br = bn % bd;
    if (a.l < b.l)
        ar <<= b.l - a.l;
    else
        br <<= a.l - b.l;
    if (ar != br)
        return ar > br;
    return ref_start(a) < ref_start(b);
}

int view_shrink(Store *s, View *v, size_t hi, size_t lo, size_t max_lines)
{
    size_t k, best;
    int changed;
    Ref a, b, parent;
    changed = 0;
    view_measure(s, v);
    if (max_lines) {
        if (v->n <= max_lines)
            return 0;
    } else {
        if (v->bytes > hi)
            v->shrinking = 1;
        if (!v->shrinking)
            return 0;
    }
    while (max_lines ? v->n > max_lines : v->bytes > lo) {
        best = v->n;
        for (k = 0; k + 1 < v->n; k++) {
            a = v->v[k];
            b = v->v[k + 1];
            if (a.l != b.l || (a.i & 1) || b.i != a.i + 1 ||
                a.l + 1 >= TREE_LEVELS)
                continue;
            parent.l = a.l + 1;
            parent.i = a.i / 2;
            if (!store_node(s, parent))
                continue;
            if (best == v->n || more_due(s->count, a, v->v[best]))
                best = k;
        }
        if (best == v->n)
            break;
        a = v->v[best];
        parent.l = a.l + 1;
        parent.i = a.i / 2;
        v->v[best] = parent;
        memmove(v->v + best + 1, v->v + best + 2,
                (v->n - best - 2) * sizeof(*v->v));
        v->n--;
        changed = 1;
        view_measure(s, v);
    }
    if (!max_lines && v->bytes <= lo)
        v->shrinking = 0;
    return changed;
}
