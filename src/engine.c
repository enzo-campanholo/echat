#include "engine.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

static unsigned long context_end(Job j)
{
    return ref_start(j.ref) + (j.ref.l ? ref_span(j.ref) : 0);
}

/* Fits the 32 KB context, judged from sizes alone. */
static int fits(Engine *e, Job j)
{
    return view_prefix(e->store, &e->compact, context_end(j), 1) <= COMPACT_HIGH;
}

/* Oversized jobs wait in a min-heap by their context's end. A built ancestor
 * cannot cross an unbuilt job, so these prefixes increase with the cutoff.
 * Only a view merge can make the smallest blocked prefix fit. */
static void defer(Engine *e, Job j)
{
    Queue *q;
    Node *n;
    size_t i, parent;
    q = &e->deferred;
    n = store_slot(e->store, j.ref);
    n->busy = 0;
    n->queued = NODE_DEFERRED;
    j.retry = 1; /* a deferred leaf is already counted in inflight */
    enqueue(q, j);
    i = q->n - 1;
    while (i) {
        parent = (i - 1) / 2;
        if (context_end(q->v[parent]) <= context_end(j))
            break;
        q->v[i] = q->v[parent];
        i = parent;
    }
    q->v[i] = j;
}

static int take_deferred(Engine *e, Job *j)
{
    Queue *q;
    Job last;
    size_t i, child;
    q = &e->deferred;
    if (!e->deferred_ready || !q->n)
        return 0;
    if (!fits(e, q->v[0])) {
        e->deferred_ready = 0;
        return 0;
    }
    *j = q->v[0];
    last = q->v[--q->n];
    i = 0;
    while (i < q->n / 2) {
        child = i * 2 + 1;
        if (child + 1 < q->n &&
            context_end(q->v[child + 1]) < context_end(q->v[child]))
            child++;
        if (context_end(last) <= context_end(q->v[child]))
            break;
        q->v[i] = q->v[child];
        i = child;
    }
    if (q->n)
        q->v[i] = last;
    return 1;
}

static void ready(Engine *e, Ref r)
{
    Node *n;
    Job j;
    n = store_slot(e->store, r);
    if (!n || n->size || n->queued || n->busy)
        return;
    n->queued = NODE_READY;
    j.ref = r;
    j.retry = 0;
    j.fails = 0;
    j.due = 0;
    enqueue(r.l ? &e->ready : &e->leaves, j);
}

static int save_compact(Engine *e)
{
    Buf b;
    int result;
    buf_init(&b);
    buf_puts(&b, "{\"shrinking\":");
    buf_ulong(&b, (unsigned long)(e->compact.shrinking != 0));
    buf_puts(&b, ",\"view\":");
    view_dump(&b, &e->compact);
    buf_puts(&b, "}\n");
    result = store_save_file(e->store, "compaction-view.json", &b);
    buf_free(&b);
    return result;
}

static int load_compact(Engine *e)
{
    char *path, *text;
    Json *j;
    unsigned long shrinking, end;
    Ref r;
    int result;
    path = path_join(e->store->dir, "compaction-view.json");
    errno = 0;
    text = read_file(path);
    free(path);
    if (!text) {
        if (errno != ENOENT)
            return store_fail(e->store, "cannot read compaction-view.json", NULL);
        view_copy(&e->compact, &e->store->view);
        e->compact.shrinking = 1;
        view_shrink(e->store, &e->compact, COMPACT_HIGH, COMPACT_LOW, 0);
        return save_compact(e);
    }
    j = json_parse(text);
    free(text);
    result = j && j->type == J_OBJ &&
             json_ulong(json_get(j, "shrinking"), &shrinking) && shrinking <= 1 &&
             !view_parse(e->store, &e->compact, json_get(j, "view")) ? 0 : -1;
    json_free(j);
    if (result)
        return store_fail(e->store, "invalid compaction-view.json; refusing to rebuild it", NULL);
    end = e->compact.n ? ref_start(e->compact.v[e->compact.n - 1]) +
                         ref_span(e->compact.v[e->compact.n - 1]) : 0;
    for (; end < e->store->count; end++) {
        r.l = 0;
        r.i = end;
        view_add(&e->compact, r);
    }
    view_measure(e->store, &e->compact);
    /* The pending marker is conservative: completing a batch early only
     * changes the small compaction prefix, never the saved turn view. */
    e->compact.shrinking = (int)shrinking || e->compact.bytes > COMPACT_HIGH;
    return 0;
}

int engine_open(Engine *e, Store *s)
{
    unsigned int l;
    unsigned long i, n;
    Ref r, left, right;
    memset(e, 0, sizeof(*e));
    e->store = s;
    view_init(&e->compact);
    if (load_compact(e) < 0)
        return -1;
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
    free(e->deferred.v);
    view_free(&e->compact);
    memset(e, 0, sizeof(*e));
}

/* Views are saved only when merges change them or a batch starts or ends:
 * appended leaves are recovered from the log at the next open. */
static int update_views(Engine *e)
{
    int changed, pending, compact_changed, compact_pending;
    Store *s;
    s = e->store;
    pending = s->view.shrinking;
    changed = view_shrink(s, &s->view, VIEW_HIGH, VIEW_LOW, 0);
    if (changed) {
        s->generation++;
        /* The compactor follows a chat batch at exactly the same boundary. */
        view_copy(&e->compact, &s->view);
        e->compact.shrinking = 1;
    }
    if ((changed || pending != s->view.shrinking) && store_save_view(s) < 0)
        return -1;
    compact_pending = e->compact.shrinking;
    compact_changed = view_shrink(s, &e->compact, COMPACT_HIGH, COMPACT_LOW, 0);
    if (changed || compact_changed)
        e->deferred_ready = 1;
    if (changed || compact_changed || compact_pending != e->compact.shrinking)
        return save_compact(e);
    return 0;
}

/* The user's words retry failed work at once; the agent's own records do
 * not, or one busy turn would repeat a failing call hundreds of times. */
int engine_sync(Engine *e)
{
    Ref r;
    int added, user;
    added = e->seen < e->store->count;
    user = 0;
    while (e->seen < e->store->count) {
        r.l = 0;
        r.i = e->seen++;
        user |= e->store->messages[r.i].kind == KIND_USER;
        ready(e, r);
        view_add(&e->compact, r);
    }
    if (!added)
        return 0;
    if (user)
        engine_retry(e);
    return update_views(e);
}

static void requeue(Engine *e, Job j)
{
    store_slot(e->store, j.ref)->queued = NODE_READY;
    j.retry = 1;
    enqueue(&e->ready, j);
}
void engine_retry(Engine *e)
{
    Job j;
    while (dequeue(&e->failed, &j)) {
        j.fails = 0;
        requeue(e, j);
    }
}

/* Failed nodes retry by themselves after 15, 60 and 300 seconds, then wait
 * for a new message or an explicit retry, so a dead provider costs little. */
int engine_tick(Engine *e, long now)
{
    Queue keep;
    Job j;
    size_t k;
    int moved;
    for (k = e->failed.head; k < e->failed.n; k++)
        if (e->failed.v[k].fails <= RETRIES && e->failed.v[k].due <= now)
            break;
    if (k == e->failed.n)
        return 0;
    memset(&keep, 0, sizeof(keep));
    moved = 0;
    while (dequeue(&e->failed, &j)) {
        if (j.fails <= RETRIES && j.due <= now) {
            requeue(e, j);
            moved++;
        } else {
            enqueue(&keep, j);
        }
    }
    free(e->failed.v);
    e->failed = keep;
    return moved;
}

long engine_deadline(Engine *e, long now)
{
    long best;
    size_t k;
    best = -1;
    for (k = e->failed.head; k < e->failed.n; k++)
        if (e->failed.v[k].fails <= RETRIES &&
            (best < 0 || e->failed.v[k].due - now < best))
            best = e->failed.v[k].due > now ? e->failed.v[k].due - now : 0;
    return best;
}

int engine_done(Engine *e, Job j, const char *text)
{
    Ref sibling, parent, leaf;
    Node *n;
    char *cut;
    size_t len;
    int result;
    /* A node far over the ruler would crowd every later context out of its
     * 32 KB budget and could never merge away: cut it at a code point. */
    cut = NULL;
    if (strlen(text) > NODE_BYTES + NODE_SLACK) {
        len = utf8_prefix(text, NODE_BYTES);
        cut = xmalloc(len + 1);
        memcpy(cut, text, len);
        cut[len] = 0;
        text = cut;
    }
    result = store_build(e->store, j.ref, text);
    free(cut);
    if (result < 0)
        return -1;
    if (e->built)
        e->built(e->opaque, j.ref);
    n = store_slot(e->store, j.ref);
    n->busy = 0;
    n->queued = NODE_IDLE;
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

void engine_fail(Engine *e, Job j, long now)
{
    static const long backoff[RETRIES] = { 15, 60, 300 };
    Node *n;
    n = store_slot(e->store, j.ref);
    n->busy = 0;
    n->queued = NODE_FAILED;
    j.fails++;
    j.due = j.fails <= RETRIES ? now + backoff[j.fails - 1] : 0;
    enqueue(&e->failed, j);
}

/* A source that fits in a line is its own node, with no model call: a short
 * message (kind: text) verbatim, two short lines joined by a newline. Sizes
 * decide; text is read only then. Returns the text, NULL if a model must
 * build it, or sets *error on a read failure. */
static char *shortcut(Engine *e, Job j, int *error)
{
    Store *s;
    Node *a, *b;
    Ref child;
    char *left, *right;
    Buf source;
    s = e->store;
    *error = 0;
    buf_init(&source);
    if (!j.ref.l) {
        if (s->messages[j.ref.i].has_images ||
            strlen(store_kind(s, j.ref.i)) + 2 + s->messages[j.ref.i].size > NODE_BYTES)
            return NULL;
        left = store_message(s, j.ref.i);
        if (!left) {
            *error = 1;
            return NULL;
        }
        buf_puts(&source, store_kind(s, j.ref.i));
        buf_puts(&source, ": ");
        buf_puts(&source, left);
        free(left);
        return source.s;
    }
    child.l = j.ref.l - 1;
    child.i = j.ref.i * 2;
    a = store_node(s, child);
    child.i++;
    b = store_node(s, child);
    if (!a || !b) {
        store_fail(s, "ready merge has an unbuilt child", NULL);
        *error = 1;
        return NULL;
    }
    if ((unsigned long)a->size + 1 + b->size > NODE_BYTES)
        return NULL;
    right = store_text(s, child);
    child.i--;
    left = right ? store_text(s, child) : NULL;
    if (!left) {
        free(right);
        *error = 1;
        return NULL;
    }
    buf_puts(&source, left);
    buf_putc(&source, '\n');
    buf_puts(&source, right);
    free(left);
    free(right);
    return source.s;
}

int engine_next(Engine *e, Job *j)
{
    Node *n;
    char *text;
    int have, error;
    for (;;) {
        have = take_deferred(e, j) || dequeue(&e->ready, j);
        if (!have) {
            if (e->inflight >= WORKERS || !dequeue(&e->leaves, j))
                return 0;
        }
        n = store_slot(e->store, j->ref);
        n->queued = NODE_IDLE;
        if (n->size || n->busy)
            continue;
        n->busy = 1;
        if (!j->ref.l && !j->retry)
            e->inflight++;
        text = shortcut(e, *j, &error);
        if (error)
            return -1;
        if (text) {
            error = engine_done(e, *j, text);
            free(text);
            if (error < 0)
                return -1;
            continue;
        }
        if (!fits(e, *j)) {
            defer(e, *j);
            continue;
        }
        return 1;
    }
}

char *engine_task(Engine *e, Job j)
{
    Buf b;
    char *text;
    Ref a;
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
        buf_puts(&b, store_kind(e->store, j.ref.i));
        buf_puts(&b, ": ");
        buf_puts(&b, text);
        free(text);
    } else {
        a.l = j.ref.l - 1;
        for (a.i = j.ref.i * 2; a.i <= j.ref.i * 2 + 1; a.i++) {
            text = store_text(e->store, a);
            if (!text) {
                buf_free(&b);
                return NULL;
            }
            buf_puts(&b, text);
            buf_puts(&b, a.i & 1 ? "" : "\n");
            free(text);
        }
    }
    buf_puts(&b, "\n</input>");
    return b.s;
}

char *engine_context(Engine *e, Job j)
{
    return view_render(e->store, &e->compact, context_end(j), 1, COMPACT_HIGH);
}

int engine_caught_up(Engine *e, unsigned long before)
{
    return e->contiguous >= before;
}
