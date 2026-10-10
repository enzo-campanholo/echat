#include "store.h"
#include <stdlib.h>
#include <string.h>

#define PLACEHOLDER "(not summarized yet: zoom it)"

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

void view_dump(Buf *b, const View *v)
{
    size_t i;
    buf_putc(b, '[');
    for (i = 0; i < v->n; i++) {
        if (i)
            buf_putc(b, ',');
        buf_putc(b, '[');
        buf_ulong(b, v->v[i].l);
        buf_putc(b, ',');
        buf_ulong(b, v->v[i].i);
        buf_putc(b, ']');
    }
    buf_putc(b, ']');
}

int view_parse(Store *s, View *v, const Json *j)
{
    const Json *p;
    unsigned long l, i, end;
    Ref r;
    end = 0;
    if (!j || j->type != J_ARR)
        return store_fail(s, "view must be an array of [level,index] pairs", NULL);
    for (p = j->child; p; p = p->next) {
        if (p->type != J_ARR || !p->child || !p->child->next ||
            p->child->next->next || !json_ulong(p->child, &l) ||
            l >= TREE_LEVELS || !json_ulong(p->child->next, &i))
            return store_fail(s, "invalid saved view pair", NULL);
        r.l = (unsigned int)l;
        r.i = i;
        if (!store_valid_ref(s, r) || ref_start(r) != end ||
            (r.l && !store_node(s, r)))
            return store_fail(s, "saved view has a gap, overlap, or unbuilt parent", NULL);
        end += ref_span(r);
        view_add(v, r);
    }
    return 0;
}

void view_line(Buf *b, Ref r, const char *text)
{
    const char *p;
    buf_ulong(b, ref_start(r));
    buf_putc(b, '+');
    buf_ulong(b, ref_span(r));
    buf_putc(b, '|');
    p = text ? text : PLACEHOLDER;
    for (; *p; p++)
        buf_putc(b, (*p == '\n' || *p == '\r') ? ' ' : *p);
    buf_putc(b, '\n');
}

/* The bytes view_line writes for a node, from its size alone. */
static size_t line_size(Store *s, Ref r)
{
    Node *node;
    unsigned long id, n;
    size_t size;
    node = store_node(s, r);
    size = node ? node->size : sizeof(PLACEHOLDER) - 1;
    size += 3; /* +, |, LF */
    id = ref_start(r);
    n = ref_span(r);
    do { size++; id /= 10; } while (id);
    do { size++; n /= 10; } while (n);
    return size;
}

/* Visit the prefix's lines before END: open only the node crossing END. Its
 * children exist; this keeps the saved view's merges and never exposes a
 * later message. built_only stops at the first unbuilt line. Returns 0 to
 * stop, -1 on a read error. With B, render; otherwise add sizes to *bytes. */
static int prefix(Store *s, Ref r, unsigned long end, int built_only, Buf *b,
                  size_t *bytes)
{
    Ref child;
    unsigned long start;
    char *text;
    int k;
    start = ref_start(r);
    if (start >= end)
        return 0;
    if (ref_span(r) > end - start) {
        child.l = r.l - 1;
        child.i = r.i * 2;
        k = prefix(s, child, end, built_only, b, bytes);
        if (k <= 0)
            return k;
        child.i++;
        return prefix(s, child, end, built_only, b, bytes);
    }
    if (!store_node(s, r)) {
        if (built_only)
            return 0;
        if (b)
            view_line(b, r, NULL);
    } else if (b) {
        text = store_text(s, r);
        if (!text)
            return -1;
        view_line(b, r, text);
        free(text);
    }
    if (!b)
        *bytes += line_size(s, r);
    return 1;
}

size_t view_prefix(Store *s, View *v, unsigned long end, int built_only)
{
    size_t i, bytes;
    bytes = sizeof("<chat>\n</chat>\n") - 1;
    for (i = 0; i < v->n; i++)
        if (prefix(s, v->v[i], end, built_only, NULL, &bytes) <= 0)
            break;
    return bytes;
}

char *view_render(Store *s, View *v, unsigned long end, int built_only,
                  size_t limit)
{
    Buf b;
    size_t i;
    int k;
    if (limit && view_prefix(s, v, end, built_only) > limit)
        return NULL;
    buf_init(&b);
    buf_puts(&b, "<chat>\n");
    for (i = 0, k = 1; k > 0 && i < v->n; i++)
        k = prefix(s, v->v[i], end, built_only, &b, NULL);
    if (k < 0) {
        buf_free(&b);
        return NULL;
    }
    buf_puts(&b, "</chat>\n");
    return b.s;
}

size_t view_measure(Store *s, View *v)
{
    size_t k, size, bytes;
    bytes = sizeof("<chat>\n</chat>\n") - 1;
    for (k = 0; k < v->n; k++) {
        size = line_size(s, v->v[k]);
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
