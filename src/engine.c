#include "engine.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void enqueue(Queue *q, Job j)
{
    if (q->head && q->head == q->n) {
        q->head = q->n = 0;
    }
    if (q->n == q->cap) {
        if (q->head) {
            memmove(q->v, q->v + q->head, (q->n - q->head) * sizeof(*q->v));
            q->n -= q->head;
            q->head = 0;
        } else {
            if (q->cap > (size_t)-1 / sizeof(*q->v) / 2) {
                fputs("unii: job queue is too large\n", stderr);
                exit(1);
            }
            q->cap = q->cap ? q->cap * 2 : 32;
            q->v = xrealloc(q->v, q->cap * sizeof(*q->v));
        }
    }
    q->v[q->n++] = j;
}

static int dequeue(Queue *q, Job *j)
{
    if (q->head == q->n)
        return 0;
    *j = q->v[q->head++];
    return 1;
}

static void ready(Engine *e, Ref r)
{
    Node *n;
    Job j;
    n = store_slot(e->store, r);
    if (!n || n->text || n->queued || n->busy)
        return;
    n->queued = 1;
    j.ref = r;
    j.retry = 0;
    enqueue(r.l ? &e->ready : &e->leaves, j);
}

static int save_compact(Engine *e)
{
    Buf b;
    char *path, *tmp;
    size_t i;
    int fd, ok;
    buf_init(&b);
    buf_puts(&b, "{\"shrinking\":");
    buf_ulong(&b, (unsigned long)e->compact.shrinking);
    buf_puts(&b, ",\"view\":[");
    for (i = 0; i < e->compact.n; i++) {
        if (i)
            buf_putc(&b, ',');
        buf_putc(&b, '[');
        buf_ulong(&b, e->compact.v[i].l);
        buf_putc(&b, ',');
        buf_ulong(&b, e->compact.v[i].i);
        buf_putc(&b, ']');
    }
    buf_puts(&b, "]}\n");
    path = path_join(e->store->dir, "compaction-view.json");
    tmp = path_join(e->store->dir, ".compaction-view.tmp");
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ok = fd >= 0 && write_all(fd, b.s, b.len) == 0 && fsync(fd) == 0;
    if (fd >= 0 && close(fd) < 0)
        ok = 0;
    if (ok && rename(tmp, path) < 0)
        ok = 0;
    if (ok) {
        fd = open(e->store->dir, O_RDONLY);
        if (fd < 0 || fsync(fd) < 0)
            ok = 0;
        if (fd >= 0)
            close(fd);
    }
    free(path);
    free(tmp);
    buf_free(&b);
    if (!ok)
        strcpy(e->store->error, "cannot save compaction view");
    return ok ? 0 : -1;
}

static int load_compact(Engine *e)
{
    char *path, *text;
    Json *j, *p, *a, *entries;
    unsigned long l, i, end, shrinking;
    Ref r;
    path = path_join(e->store->dir, "compaction-view.json");
    text = read_file(path);
    free(path);
    if (!text) {
        if (errno != ENOENT)
            return -1;
        view_copy(&e->compact, &e->store->view);
        e->compact.shrinking = 1;
        view_shrink(e->store, &e->compact, COMPACT_HIGH, COMPACT_LOW, 0);
        return save_compact(e);
    }
    j = json_parse(text);
    free(text);
    end = 0;
    entries = json_get(j, "view");
    if (!j || j->type != J_OBJ || !entries || entries->type != J_ARR ||
        !json_ulong(json_get(j, "shrinking"), &shrinking) || shrinking > 1)
        goto bad;
    for (p = entries->child; p; p = p->next) {
        a = p->child;
        if (p->type != J_ARR || !a || !a->next || a->next->next ||
            !json_ulong(a, &l) || l >= TREE_LEVELS ||
            !json_ulong(a->next, &i))
            goto bad;
        r.l = (unsigned int)l;
        r.i = i;
        if (!store_valid_ref(e->store, r) || ref_start(r) != end)
            goto bad;
        end += ref_span(r);
        view_add(&e->compact, r);
    }
    while (end < e->store->count) {
        r.l = 0;
        r.i = end++;
        view_add(&e->compact, r);
    }
    json_free(j);
    view_measure(e->store, &e->compact);
    /* The pending marker is conservative: completing a batch early only
     * changes the small compaction prefix, never the saved turn view. */
    e->compact.shrinking = (int)shrinking || e->compact.bytes > COMPACT_HIGH;
    return 0;
bad:
    json_free(j);
    strcpy(e->store->error, "invalid compaction-view.json; refusing to rebuild it");
    return -1;
}

int engine_open(Engine *e, Store *s)
{
    unsigned int l;
    unsigned long i, n;
    Ref r, left, right;
    memset(e, 0, sizeof(*e));
    e->store = s;
    view_init(&e->compact);
    if (load_compact(e) < 0) {
        if (!s->error[0])
            strcpy(s->error, "cannot load compaction view");
        return -1;
    }
    /* One linear startup pass. From here on, completion enqueues parents. */
    for (i = 0; i < s->count; i++) {
        r.l = 0;
        r.i = i;
        if (!store_node(s, r))
            ready(e, r);
    }
    for (l = 1; l < TREE_LEVELS; l++) {
        n = s->count >> l;
        if (!n)
            break;
        for (i = 0; i < n; i++) {
            r.l = l;
            r.i = i;
            left.l = right.l = l - 1;
            left.i = i * 2;
            right.i = i * 2 + 1;
            if (!store_node(s, r) && store_node(s, left) && store_node(s, right))
                ready(e, r);
        }
    }
    e->seen = s->count;
    while (e->contiguous < s->count) {
        r.l = 0;
        r.i = e->contiguous;
        if (!store_node(s, r))
            break;
        e->contiguous++;
    }
    return 0;
}

void engine_close(Engine *e)
{
    free(e->ready.v);
    free(e->leaves.v);
    free(e->failed.v);
    view_free(&e->compact);
    memset(e, 0, sizeof(*e));
}

static int update_views(Engine *e)
{
    int changed, pending, compact_changed, compact_pending;
    unsigned long old_generation;
    Store *s;
    s = e->store;
    pending = s->view.shrinking;
    old_generation = s->generation;
    changed = view_shrink(s, &s->view, VIEW_HIGH, VIEW_LOW, 0);
    if (changed) {
        s->generation++;
        /* The compactor follows a chat batch at exactly the same boundary. */
        view_copy(&e->compact, &s->view);
        e->compact.shrinking = 1;
    }
    if (changed || pending != s->view.shrinking) {
        if (!store_save_view(s))
            return -1;
    }
    compact_pending = e->compact.shrinking;
    compact_changed = view_shrink(s, &e->compact, COMPACT_HIGH, COMPACT_LOW, 0);
    if (old_generation != s->generation || changed || compact_changed ||
        compact_pending != e->compact.shrinking)
        return save_compact(e);
    return 0;
}

int engine_sync(Engine *e)
{
    Ref r;
    int added;
    added = e->seen < e->store->count;
    while (e->seen < e->store->count) {
        r.l = 0;
        r.i = e->seen++;
        ready(e, r);
        view_add(&e->compact, r);
    }
    if (added) {
        engine_retry(e);
        if (update_views(e) < 0 || save_compact(e) < 0)
            return -1;
    }
    return 0;
}

void engine_retry(Engine *e)
{
    Job j;
    while (dequeue(&e->failed, &j)) {
        store_slot(e->store, j.ref)->queued = 1;
        j.retry = 1;
        enqueue(&e->ready, j);
    }
}

int engine_done(Engine *e, Job j, const char *text)
{
    Ref sibling, parent, leaf;
    Node *n;
    if (!store_build(e->store, j.ref, text))
        return -1;
    n = store_slot(e->store, j.ref);
    n->busy = n->queued = 0;
    if (!j.ref.l && e->inflight)
        e->inflight--;
    sibling = j.ref;
    sibling.i ^= 1;
    parent.l = j.ref.l + 1;
    parent.i = j.ref.i / 2;
    if (parent.l < TREE_LEVELS && store_valid_ref(e->store, parent) &&
        store_node(e->store, sibling))
        ready(e, parent);
    while (e->contiguous < e->store->count) {
        leaf.l = 0;
        leaf.i = e->contiguous;
        if (!store_node(e->store, leaf))
            break;
        e->contiguous++;
    }
    return update_views(e);
}

void engine_fail(Engine *e, Job j)
{
    Node *n;
    n = store_slot(e->store, j.ref);
    n->busy = 0;
    n->queued = 2;
    enqueue(&e->failed, j);
}

int engine_next(Engine *e, Job *j)
{
    Node *n, *a, *b;
    Ref child;
    char *text;
    Buf source;
    int have;
    for (;;) {
        have = dequeue(&e->ready, j);
        if (!have) {
            if (e->inflight >= WORKERS || !dequeue(&e->leaves, j))
                return 0;
        }
        n = store_slot(e->store, j->ref);
        n->queued = 0;
        if (n->text || n->busy)
            continue;
        n->busy = 1;
        if (!j->ref.l && !j->retry)
            e->inflight++;
        buf_init(&source);
        if (!j->ref.l) {
            text = store_message(e->store, j->ref.i);
            if (!text) {
                buf_free(&source);
                return -1;
            }
            buf_puts(&source, e->store->messages[j->ref.i].kind);
            buf_puts(&source, ": ");
            buf_puts(&source, text);
            free(text);
        } else {
            child.l = j->ref.l - 1;
            child.i = j->ref.i * 2;
            a = store_node(e->store, child);
            child.i++;
            b = store_node(e->store, child);
            if (!a || !b) {
                strcpy(e->store->error, "ready merge has an unbuilt child");
                buf_free(&source);
                return -1;
            }
            buf_puts(&source, a->text);
            buf_putc(&source, '\n');
            buf_puts(&source, b->text);
        }
        if (source.len <= NODE_BYTES &&
            (j->ref.l || !e->store->messages[j->ref.i].has_images)) {
            if (engine_done(e, *j, source.s) < 0) {
                buf_free(&source);
                return -1;
            }
            buf_free(&source);
            continue;
        }
        buf_free(&source);
        return 1;
    }
}

char *engine_task(Engine *e, Job j)
{
    Buf b;
    char *text;
    Ref a;
    Node *n;
    size_t k;
    buf_init(&b);
    if (!j.ref.l) {
        buf_puts(&b, "Compaction: compress message ");
        buf_ulong(&b, j.ref.i);
        buf_puts(&b, " into one line of at most 512 bytes\n(about 70 words), the length of this ruler:\n");
    } else {
        buf_puts(&b, "Compaction: merge lines ");
        buf_ulong(&b, ref_start(j.ref));
        buf_putc(&b, '+');
        buf_ulong(&b, ref_span(j.ref) / 2);
        buf_puts(&b, " and ");
        buf_ulong(&b, ref_start(j.ref) + ref_span(j.ref) / 2);
        buf_putc(&b, '+');
        buf_ulong(&b, ref_span(j.ref) / 2);
        buf_puts(&b, ", adjacent, into one line of at most\n512 bytes (about 70 words), the length of this ruler:\n");
    }
    for (k = 0; k < NODE_BYTES; k++)
        buf_putc(&b, '-');
    buf_putc(&b, '\n');
    if (j.ref.l) {
        buf_puts(&b, "<chat> may hold their messages, ");
        buf_ulong(&b, ref_start(j.ref));
        buf_puts(&b, " to ");
        buf_ulong(&b, ref_start(j.ref) + ref_span(j.ref) - 1);
        buf_puts(&b, ", in more detail: take details\nof them from there too.\n");
    }
    buf_puts(&b, "<input>\n");
    if (!j.ref.l) {
        text = store_message(e->store, j.ref.i);
        if (!text) {
            buf_free(&b);
            return NULL;
        }
        buf_puts(&b, e->store->messages[j.ref.i].kind);
        buf_puts(&b, ": ");
        buf_puts(&b, text);
        free(text);
    } else {
        a.l = j.ref.l - 1;
        a.i = j.ref.i * 2;
        n = store_node(e->store, a);
        buf_puts(&b, n->text);
        buf_putc(&b, '\n');
        a.i++;
        n = store_node(e->store, a);
        buf_puts(&b, n->text);
    }
    buf_puts(&b, "\n</input>");
    return b.s;
}

char *engine_context(Engine *e, Job j)
{
    unsigned long end;
    end = ref_start(j.ref) + (j.ref.l ? ref_span(j.ref) : 0);
    return view_render(e->store, &e->compact, end, 1);
}

int engine_caught_up(Engine *e, unsigned long before)
{
    return e->contiguous >= before;
}
