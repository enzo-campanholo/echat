#include "engine.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef UNII_ASSETS
#define UNII_ASSETS "pi"
#endif

#define PROTOCOL 1
#define SPEC_PENDING "A\nmessage not summarized yet shows as \"(not summarized yet: zoom it)\"."
#define TAIL_PENDING "Messages\nnot summarized yet follow the view whole, inside <recent> tags."

/* One outstanding model call per slot. The host performs it statelessly:
 * each attempt resends the whole conversation, corrections included. */
typedef struct Call {
    Job job;
    unsigned long id; /* 0: idle */
    int attempts;
    long started; /* monotonic seconds */
    char *shortest;
    Buf messages; /* comma-separated message objects */
} Call;

/* Cache gating per requester: [0] is the turn, [k + 1] is call slot k. A held
 * key's request is writing that prefix and has no response yet; a requester
 * with the same key waits, so the prefix is paid for once. */
typedef struct Gate { char *hold, *key; Json *wait; } Gate;

typedef struct Server {
    Store store;
    Engine engine;
    Call calls[WORKERS];
    Gate gates[WORKERS + 1];
    const char *provider, *model, *effort;
    long timeout; /* seconds per call */
    unsigned long serial, before;
    Json *pending; /* turn or flush waiting for memory */
    unsigned long waiting; /* last progress reported; ULONG_MAX: none yet */
    int ready, events, quitting, fatal;
    Buf input;
} Server;

#define HISTORY_BYTES 1048576 /* messages response budget */

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

/* Timeouts survive wall-clock steps. */
static long seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
        return 0;
    return (long)t.tv_sec;
}

static const char *env_default(const char *key, const char *fallback)
{
    const char *s;
    s = getenv(key);
    return s && *s ? s : fallback;
}

static void emit(Buf *b)
{
    if (write_all(STDOUT_FILENO, b->s, b->len) < 0)
        interrupted = 1;
    buf_free(b);
}

/* Every response is {"id":ID,"ok":...} plus fields; keys are plain ASCII. */
static void begin(Buf *b, const Json *req, int ok)
{
    const Json *id;
    buf_init(b);
    buf_puts(b, "{\"id\":");
    id = json_get(req, "id");
    if (id)
        json_dump(b, id);
    else
        buf_puts(b, "null");
    buf_puts(b, ok > 0 ? ",\"ok\":true" : ok < 0 ? ",\"ok\":false,\"fatal\":true" :
                ",\"ok\":false");
}

static void key_str(Buf *b, const char *key, const char *value)
{
    buf_puts(b, ",\"");
    buf_puts(b, key);
    buf_puts(b, "\":");
    buf_json(b, value);
}

static void key_ulong(Buf *b, const char *key, unsigned long n)
{
    buf_puts(b, ",\"");
    buf_puts(b, key);
    buf_puts(b, "\":");
    buf_ulong(b, n);
}

static void finish(Buf *b)
{
    buf_puts(b, "}\n");
    emit(b);
}

static void response(const Json *req, int ok, const char *field, const char *value)
{
    Buf b;
    begin(&b, req, ok);
    if (field)
        key_str(&b, field, value ? value : "");
    finish(&b);
}

static void error_response(const Json *req, const char *error)
{
    response(req, 0, "error", error);
}

static void storage_error(Server *s, const Json *req)
{
    s->fatal = 1;
    response(req, -1, "error", s->store.error);
}

/* zoom and date for the model. Bad arguments and ranges are ordinary errors
 * it can correct; only an unreadable log (-1) is fatal. A zoom response also
 * carries "log": a short reference to record instead of the result. */
static int memory(Server *s, const Json *req, const char *op, const Json *args)
{
    unsigned long id, n, page;
    char *date;
    char *text, *images, *ref;
    Buf b, note;
    if (!strcmp(op, "date")) {
        if (!json_ulong(json_get(args, "id"), &id)) {
            response(req, 1, "text", "date: id must be a nonnegative integer");
            return 0;
        }
        date = store_date(&s->store, id);
        if (!date && s->store.broken)
            return -1;
        response(req, 1, "text", date ? date : "date: no such message");
        free(date);
        return 0;
    }
    n = 1;
    page = 0;
    if (!json_ulong(json_get(args, "id"), &id) ||
        (json_get(args, "n") && !json_ulong(json_get(args, "n"), &n)) ||
        (json_get(args, "page") && !json_ulong(json_get(args, "page"), &page))) {
        response(req, 1, "text", "zoom: id, n and page must be nonnegative integers");
        return 0;
    }
    text = store_zoom(&s->store, id, n, page, &ref);
    if (!text) {
        if (s->store.broken)
            return -1;
        error_response(req, s->store.error);
        return 0;
    }
    images = n == 1 && !page ? store_images(&s->store, id) : NULL;
    if (s->store.broken) {
        free(text);
        free(ref);
        return -1;
    }
    buf_init(&note);
    buf_puts(&note, "zoom {\"id\":");
    buf_ulong(&note, id);
    buf_puts(&note, ",\"n\":");
    buf_ulong(&note, n);
    buf_puts(&note, ",\"page\":");
    buf_ulong(&note, page);
    buf_puts(&note, "}: ");
    buf_puts(&note, ref);
    begin(&b, req, 1);
    key_str(&b, "text", text);
    if (images) {
        buf_puts(&b, ",\"images\":");
        buf_puts(&b, images);
    }
    key_str(&b, "log", note.s);
    finish(&b);
    buf_free(&note);
    free(text);
    free(ref);
    free(images);
    return 0;
}

static int running(Server *s)
{
    int k, n;
    n = 0;
    for (k = 0; k < WORKERS; k++)
        n += s->calls[k].id != 0;
    return n;
}

static void status(Server *s, const Json *req)
{
    Buf b;
    begin(&b, req, 1);
    key_ulong(&b, "messages", s->store.count);
    key_ulong(&b, "summarized", s->engine.contiguous);
    key_ulong(&b, "view_bytes", (unsigned long)view_measure(&s->store, &s->store.view));
    key_ulong(&b, "lines", (unsigned long)s->store.view.n);
    key_ulong(&b, "workers", (unsigned long)running(s));
    key_ulong(&b, "failed", (unsigned long)(s->engine.failed.n - s->engine.failed.head));
    key_ulong(&b, "blocked", (unsigned long)s->engine.deferred.n);
    if (s->model) {
        key_str(&b, "summary_provider", s->provider);
        key_str(&b, "summary_model", s->model);
    }
    finish(&b);
}

/* Log entries in one write. The append event names their kind when all
 * share one. */
static int append(Server *s, const Entry *e, size_t n, unsigned long *first)
{
    Buf b;
    unsigned long count;
    size_t k;
    if (store_append(&s->store, e, n, first, &count) < 0)
        return -1;
    if (s->events) {
        buf_init(&b);
        buf_puts(&b, "{\"event\":\"append\"");
        key_ulong(&b, "first", *first);
        key_ulong(&b, "count", count);
        for (k = 1; k < n && !strcmp(e[k].kind, e[0].kind); k++)
            ;
        if (k == n)
            key_str(&b, "kind", e[0].kind);
        finish(&b);
    }
    return engine_sync(&s->engine);
}

static void built(void *opaque, Ref r)
{
    Server *s;
    Buf b;
    s = opaque;
    if (!s->events)
        return;
    buf_init(&b);
    buf_puts(&b, "{\"event\":\"built\"");
    key_ulong(&b, "l", r.l);
    key_ulong(&b, "i", r.i);
    finish(&b);
}

/* Records [from, from + count): at least one, then up to a byte budget.
 * Images only when asked; "next" is the first id not returned. */
static int history(Server *s, const Json *req)
{
    unsigned long from, count, end, budget, i;
    const Json *flag, *images;
    Json *j;
    Buf b;
    count = 100;
    budget = HISTORY_BYTES;
    if (!json_ulong(json_get(req, "from"), &from) ||
        (json_get(req, "count") && !json_ulong(json_get(req, "count"), &count)) ||
        (json_get(req, "bytes") && !json_ulong(json_get(req, "bytes"), &budget))) {
        error_response(req, "messages needs from, and optional count and bytes");
        return 0;
    }
    if (budget > HISTORY_BYTES)
        budget = HISTORY_BYTES;
    flag = json_get(req, "images");
    end = from < s->store.count ? s->store.count : from;
    if (count < end - from)
        end = from + count;
    begin(&b, req, 1);
    buf_puts(&b, ",\"records\":[");
    for (i = from; i < end && (i == from || b.len < budget); i++) {
        j = store_record(&s->store, i);
        if (!j) {
            buf_free(&b);
            return -1;
        }
        if (i > from)
            buf_putc(&b, ',');
        buf_puts(&b, "{\"i\":");
        buf_ulong(&b, i);
        key_str(&b, "kind", json_str(json_get(j, "kind")));
        key_str(&b, "date", json_str(json_get(j, "date")));
        key_ulong(&b, "size", (unsigned long)s->store.messages[i].size);
        key_str(&b, "text", json_str(json_get(j, "text")));
        images = json_get(j, "images");
        buf_puts(&b, images && images->child ? ",\"has_images\":true" : ",\"has_images\":false");
        if (images && images->child && flag && flag->type == J_BOOL && !strcmp(flag->s, "true")) {
            buf_puts(&b, ",\"images\":");
            json_dump(&b, images);
        }
        buf_putc(&b, '}');
        json_free(j);
    }
    buf_putc(&b, ']');
    key_ulong(&b, "next", i);
    finish(&b);
    return 0;
}

static Gate *gate(Server *s, const Json *req)
{
    unsigned long id;
    int k;
    if (!json_get(req, "call"))
        return &s->gates[0];
    if (!json_ulong(json_get(req, "call"), &id) || !id)
        return NULL;
    for (k = 0; k < WORKERS; k++)
        if (s->calls[k].id == id)
            return &s->gates[k + 1];
    return NULL;
}

static int held(Server *s, const char *key)
{
    int k;
    for (k = 0; k <= WORKERS; k++)
        if (s->gates[k].hold && !strcmp(s->gates[k].hold, key))
            return 1;
    return 0;
}

/* A started response wrote the entry, so every waiter can read it. An
 * abandoned request wrote nothing: its first waiter becomes the writer. */
static void release(Server *s, Gate *g, int written)
{
    char *key;
    Gate *w;
    int k;
    key = g->hold;
    g->hold = NULL;
    if (!key)
        return;
    for (k = 0; k <= WORKERS; k++) {
        w = &s->gates[k];
        if (!w->wait || strcmp(w->key, key))
            continue;
        if (written)
            free(w->key);
        else
            w->hold = w->key;
        w->key = NULL;
        response(w->wait, 1, NULL, NULL);
        json_free(w->wait);
        w->wait = NULL;
        if (!written)
            break;
    }
    free(key);
}

static void cache_wait(Server *s, Json *req)
{
    Gate *g;
    const char *key;
    g = gate(s, req);
    key = json_str(json_get(req, "key"));
    if (!g || !key || !*key || g->wait) {
        error_response(req, "invalid or duplicate cache wait");
        json_free(req);
        return;
    }
    release(s, g, 0); /* this requester's previous request has ended */
    if (held(s, key)) {
        g->key = xstrdup(key);
        g->wait = req;
        return;
    }
    g->hold = xstrdup(key);
    response(req, 1, NULL, NULL);
    json_free(req);
}

static void call_event(Server *s, Call *c)
{
    Buf b;
    c->id = ++s->serial;
    c->started = seconds();
    buf_init(&b);
    buf_puts(&b, "{\"event\":\"call\"");
    key_ulong(&b, "call", c->id);
    key_str(&b, "provider", s->provider);
    key_str(&b, "model", s->model);
    key_str(&b, "effort", s->effort);
    buf_puts(&b, ",\"messages\":[");
    buf_add(&b, c->messages.s, c->messages.len);
    buf_puts(&b, "]}\n");
    emit(&b);
}

static void message(Buf *b, const char *role, const char *text, const char *images)
{
    if (b->len)
        buf_putc(b, ',');
    buf_puts(b, "{\"role\":");
    buf_json(b, role);
    buf_puts(b, ",\"text\":");
    buf_json(b, text);
    if (images) {
        buf_puts(b, ",\"images\":");
        buf_puts(b, images);
    }
    buf_putc(b, '}');
}

static void end_call(Server *s, Call *c, int written)
{
    Gate *g;
    g = &s->gates[c - s->calls + 1];
    release(s, g, written);
    if (g->wait) {
        error_response(g->wait, "call ended");
        json_free(g->wait);
        free(g->key);
        g->wait = NULL;
        g->key = NULL;
    }
    free(c->shortest);
    c->shortest = NULL;
    c->id = 0;
    buf_reset(&c->messages);
}

static void failed_job(Server *s, Call *c, const char *why)
{
    Buf b;
    if (s->events) {
        buf_init(&b);
        buf_puts(&b, "{\"event\":\"error\"");
        key_ulong(&b, "call", c->id);
        key_ulong(&b, "l", c->job.ref.l);
        key_ulong(&b, "i", c->job.ref.i);
        key_str(&b, "provider", s->provider);
        key_str(&b, "model", s->model);
        key_str(&b, "text", why);
        finish(&b);
    } else {
        fprintf(stderr, "unii: compaction %lu+%lu failed: %s\n",
                ref_start(c->job.ref), ref_span(c->job.ref), why);
    }
    engine_fail(&s->engine, c->job, seconds());
    end_call(s, c, 0);
}

/* Keep the shortest of at most ATTEMPTS answers; correct in the same
 * conversation, cutting the over-long line at the limit. */
static int answer(Server *s, Call *c, const char *line)
{
    Buf correction;
    size_t length;
    int result;
    if (!*line) {
        failed_job(s, c, "empty summary");
        return 0;
    }
    length = strlen(line);
    if (!c->shortest || length < strlen(c->shortest)) {
        free(c->shortest);
        c->shortest = xstrdup(line);
    }
    c->attempts++;
    release(s, &s->gates[c - s->calls + 1], 1);
    if (length <= NODE_BYTES || c->attempts >= ATTEMPTS) {
        result = engine_done(&s->engine, c->job, c->shortest);
        end_call(s, c, 1);
        return result;
    }
    buf_init(&correction);
    buf_puts(&correction, "Too long: your line is ");
    buf_ulong(&correction, (unsigned long)length);
    buf_puts(&correction, " bytes, over the 512-byte limit. Write\nthe whole line again for the same <input>, cutting just enough of the\nleast valuable items to fit before this cut:\n");
    buf_add(&correction, line, utf8_prefix(line, NODE_BYTES));
    buf_puts(&correction, "| \342\206\220 LIMIT");
    message(&c->messages, "assistant", line, NULL);
    message(&c->messages, "user", correction.s, NULL);
    buf_free(&correction);
    call_event(s, c);
    return 0;
}

static int start_call(Server *s, Call *c)
{
    Buf text;
    char *task, *view, *images;
    int leaf;
    leaf = !c->job.ref.l;
    task = engine_task(&s->engine, c->job);
    view = engine_context(&s->engine, c->job);
    images = leaf ? store_images(&s->store, c->job.ref.i) : NULL;
    if (s->store.broken) {
        free(task);
        free(view);
        free(images);
        return -1;
    }
    c->attempts = 0;
    if (!task || !view || (leaf && s->store.messages[c->job.ref.i].has_images && !images)) {
        failed_job(s, c, "cannot build compaction input");
    } else {
        buf_init(&text);
        buf_puts(&text, view);
        buf_puts(&text, "\n\n");
        buf_puts(&text, task);
        buf_reset(&c->messages);
        message(&c->messages, "user", text.s, images);
        buf_free(&text);
        call_event(s, c);
    }
    free(task);
    free(view);
    free(images);
    return 0;
}

/* While a turn or flush waits, report how many earlier messages still lack
 * a summary, on change only, so the wait is never silent. */
static void progress(Server *s)
{
    Buf b;
    unsigned long waiting;
    waiting = s->before > s->engine.contiguous ? s->before - s->engine.contiguous : 0;
    if (!s->events || waiting == s->waiting)
        return;
    s->waiting = waiting;
    buf_init(&b);
    buf_puts(&b, "{\"event\":\"progress\"");
    key_ulong(&b, "waiting", waiting);
    finish(&b);
}

static void recent_head(Buf *b, unsigned long i, const char *kind)
{
    buf_puts(b, "[message ");
    buf_ulong(b, i);
    buf_puts(b, " kind=");
    buf_puts(b, kind);
    buf_puts(b, "]\n");
}

static void recent_images(Buf *b, unsigned long i)
{
    buf_puts(b, "(has images: zoom(");
    buf_ulong(b, i);
    buf_puts(b, ",1))\n");
}

/* Spec change C2: rather than wait for the summaries of the messages before
 * the turn, send them whole after the view, if they fit in TAIL_BYTES. The
 * view stops at the first unbuilt leaf, so it still holds only summaries and
 * never part of a message; no built node crosses that leaf, so this view is
 * a prefix of the caught-up one and the cached prefix holds. Returns 1 if
 * answered, 0 if the turn must wait, -1 on a storage failure. */
static int answer_recent(Server *s, const Json *req)
{
    Buf b, scratch;
    char *view, *text;
    unsigned long cut, i;
    size_t bytes;
    cut = s->engine.contiguous;
    if (!TAIL_BYTES || cut >= s->before)
        return 0;
    buf_init(&scratch);
    bytes = sizeof("<recent>\n</recent>\n") - 1;
    for (i = cut; i < s->before && bytes <= TAIL_BYTES; i++) {
        buf_reset(&scratch);
        recent_head(&scratch, i, store_kind(&s->store, i));
        if (s->store.messages[i].has_images)
            recent_images(&scratch, i);
        bytes += scratch.len + s->store.messages[i].size + 1;
    }
    buf_free(&scratch);
    if (bytes > TAIL_BYTES)
        return 0;
    view = view_render(&s->store, &s->store.view, cut, 1, VIEW_HIGH);
    if (!view)
        return s->store.broken ? -1 : 0;
    buf_init(&scratch);
    buf_puts(&scratch, "<recent>\n");
    for (i = cut; i < s->before; i++) {
        text = store_message(&s->store, i);
        if (!text) {
            free(view);
            buf_free(&scratch);
            return -1;
        }
        recent_head(&scratch, i, store_kind(&s->store, i));
        buf_puts(&scratch, text);
        buf_putc(&scratch, '\n');
        if (s->store.messages[i].has_images)
            recent_images(&scratch, i);
        free(text);
    }
    buf_puts(&scratch, "</recent>\n");
    begin(&b, req, 1);
    key_str(&b, "view", view);
    key_str(&b, "recent", scratch.s);
    finish(&b);
    free(view);
    buf_free(&scratch);
    return 1;
}

static int settle_pending(Server *s)
{
    Json *req;
    const char *op;
    char *view;
    int rc;
    if (!s->pending)
        return 0;
    req = s->pending;
    op = json_str(json_get(req, "op"));
    if (!engine_caught_up(&s->engine, s->before) && !strcmp(op, "turn") &&
        (rc = answer_recent(s, req)) != 0) {
        if (rc < 0)
            return -1;
    } else if (!engine_caught_up(&s->engine, s->before)) {
        if (running(s) || (s->engine.failed.n == s->engine.failed.head &&
            (!s->engine.deferred.n || s->engine.deferred_ready ||
             s->engine.ready.head != s->engine.ready.n))) {
            progress(s);
            return 0;
        }
        /* Do not call a turn on incomplete memory. Failed nodes retry by
         * themselves; a new message or an explicit retry starts them now. */
        error_response(req, !strcmp(op, "turn") ?
            "compaction blocked or failed; input is saved; send a message or retry" :
            "compaction blocked or failed; send a message or retry");
    } else if (!strcmp(op, "flush")) {
        if (running(s) || s->engine.ready.head != s->engine.ready.n ||
            s->engine.leaves.head != s->engine.leaves.n) {
            progress(s);
            return 0;
        }
        if (s->engine.failed.head != s->engine.failed.n || s->engine.deferred.n)
            error_response(req, "some compactions blocked or failed; send a message or retry");
        else
            response(req, 1, NULL, NULL);
    } else {
        view = view_render(&s->store, &s->store.view, s->before, 1, VIEW_HIGH);
        if (!view && s->store.broken)
            return -1;
        if (!view) {
            if (running(s) || s->engine.ready.head != s->engine.ready.n ||
                (s->engine.deferred.n && s->engine.deferred_ready)) {
                progress(s);
                return 0;
            }
            error_response(req, "memory view exceeds 128000 bytes; input is saved; send a message or retry");
        } else {
            response(req, 1, "view", view);
            free(view);
        }
    }
    json_free(req);
    s->pending = NULL;
    return 0;
}

/* The host aborts its request; a late result is refused as unknown. */
static void cancel(Server *s, Call *c)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"event\":\"cancel\"");
    key_ulong(&b, "call", c->id);
    finish(&b);
    failed_job(s, c, "call timed out");
}

static int step(Server *s)
{
    Call *c;
    long now;
    int k, next;
    if (s->store.broken)
        return -1;
    now = seconds();
    for (k = 0; k < WORKERS; k++) {
        c = &s->calls[k];
        if (c->id && now - c->started > s->timeout)
            cancel(s, c);
    }
    engine_tick(&s->engine, now);
    for (k = 0; s->ready && k < WORKERS; k++) {
        c = &s->calls[k];
        if (c->id)
            continue;
        next = engine_next(&s->engine, &c->job);
        if (next < 0)
            return -1;
        if (!next)
            break;
        if (start_call(s, c) < 0)
            return -1;
    }
    return settle_pending(s);
}

/* Milliseconds until the earliest call deadline, at most one second (which
 * also bounds the latency of automatic retries). */
static int deadline(Server *s)
{
    long now, left, best;
    int k;
    now = seconds();
    best = 1;
    for (k = 0; k < WORKERS; k++) {
        if (!s->calls[k].id)
            continue;
        left = s->calls[k].started + s->timeout + 1 - now;
        if (left < best)
            best = left > 0 ? left : 0;
    }
    return (int)best * 1000;
}

static void hello(Server *s, const Json *req)
{
    Buf b;
    s->ready = s->events = 1;
    begin(&b, req, 1);
    key_ulong(&b, "protocol", PROTOCOL);
    key_ulong(&b, "messages", s->store.count);
    key_ulong(&b, "summarized", s->engine.contiguous);
    key_str(&b, "provider", s->provider);
    key_str(&b, "model", s->model);
    key_str(&b, "effort", s->effort);
    finish(&b);
}

/* An append's entries: the request itself, or its "records" array. */
static Entry *entries(const Json *req, int steer, size_t *n)
{
    const Json *list, *p;
    Entry *e;
    size_t k;
    list = steer ? NULL : json_get(req, "records");
    if (list) {
        for (k = 0, p = list->type == J_ARR ? list->child : NULL; p; p = p->next)
            k++;
        if (!k)
            return NULL;
        e = xmalloc(k * sizeof(*e));
        for (k = 0, p = list->child; p; p = p->next, k++) {
            e[k].kind = json_str(json_get(p, "kind"));
            e[k].text = json_str(json_get(p, "text"));
            e[k].images = json_get(p, "images");
            if (!e[k].kind || !e[k].text) {
                free(e);
                return NULL;
            }
        }
        *n = k;
        return e;
    }
    e = xmalloc(sizeof(*e));
    e->kind = steer ? "user" : json_str(json_get(req, "kind"));
    e->text = json_str(json_get(req, "text"));
    e->images = json_get(req, "images");
    if (!e->kind || !e->text) {
        free(e);
        return NULL;
    }
    *n = 1;
    return e;
}

static Call *find_call(Server *s, const Json *req)
{
    unsigned long id;
    int k;
    if (!json_ulong(json_get(req, "call"), &id) || !id)
        return NULL;
    for (k = 0; k < WORKERS; k++)
        if (s->calls[k].id == id)
            return &s->calls[k];
    return NULL;
}

static int dispatch(Server *s, Json *req)
{
    const char *op, *text, *kind;
    Json *j;
    Buf b;
    Call *c;
    Gate *g;
    unsigned long first;
    Entry entry, *e;
    size_t n;
    op = json_str(json_get(req, "op"));
    if (!op) {
        error_response(req, "missing operation");
    } else if (!strcmp(op, "hello")) {
        hello(s, req);
    } else if (!strcmp(op, "configure")) {
        /* Protocol 0: the host now owns the system prompt and tools. */
        j = json_get(req, "events");
        s->ready = 1;
        s->events = j && j->type == J_BOOL && !strcmp(j->s, "true");
        response(req, 1, NULL, NULL);
    } else if (!strcmp(op, "turn") || !strcmp(op, "flush")) {
        text = json_str(json_get(req, "text"));
        if (s->pending && text && !strcmp(op, "turn") &&
            !strcmp(json_str(json_get(s->pending, "op")), "flush")) {
            /* A turn supersedes a waiting flush: typed input is never refused. */
            error_response(s->pending, "flush superseded by a turn");
            json_free(s->pending);
            s->pending = NULL;
        }
        if (!s->ready) {
            error_response(req, "say hello before starting a turn");
        } else if (s->pending) {
            error_response(req, "another turn is waiting for memory");
        } else if (!strcmp(op, "turn") && !text) {
            error_response(req, "turn needs text");
        } else {
            s->before = s->store.count;
            s->waiting = ULONG_MAX;
            /* Record input before waiting, but retain the preceding boundary.
             * Rendering follows live batches while opening any crossing node. */
            entry.kind = "user";
            entry.text = text;
            entry.images = json_get(req, "images");
            if (!strcmp(op, "turn") && append(s, &entry, 1, &first) < 0) {
                if (!s->store.broken) {
                    error_response(req, s->store.error);
                    json_free(req);
                    return 0;
                }
                storage_error(s, req);
                json_free(req);
                return -1;
            }
            s->pending = req;
            if (settle_pending(s) < 0) {
                storage_error(s, s->pending);
                return -1;
            }
            return 0;
        }
    } else if (!strcmp(op, "result")) {
        c = find_call(s, req);
        if (!c) {
            error_response(req, "unknown or cancelled call");
        } else {
            response(req, 1, NULL, NULL);
            text = json_str(json_get(req, "text"));
            kind = json_str(json_get(req, "error"));
            if (kind || !text)
                failed_job(s, c, kind ? kind : "call result has no text");
            else if (answer(s, c, text) < 0)
                storage_error(s, s->pending);
        }
    } else if (!strcmp(op, "cache_wait")) {
        cache_wait(s, req);
        return 0;
    } else if (!strcmp(op, "cache_started")) {
        g = gate(s, req);
        text = json_str(json_get(req, "key"));
        if (g && g->hold && text && !strcmp(g->hold, text))
            release(s, g, 1);
        response(req, 1, NULL, NULL);
    } else if (!strcmp(op, "append") || !strcmp(op, "steer")) {
        e = entries(req, !strcmp(op, "steer"), &n);
        if (!e) {
            error_response(req, "append needs kind and text, or records");
        } else if (append(s, e, n, &first) < 0) {
            if (s->store.broken)
                storage_error(s, req);
            else
                error_response(req, s->store.error);
        } else {
            begin(&b, req, 1);
            key_ulong(&b, "message_id", first);
            finish(&b);
        }
        free(e);
    } else if (!strcmp(op, "zoom") || !strcmp(op, "date")) {
        /* Only "args": a top-level "id" is the request's correlation id. */
        if (memory(s, req, op, json_get(req, "args")) < 0)
            storage_error(s, req);
    } else if (!strcmp(op, "messages")) {
        if (history(s, req) < 0)
            storage_error(s, req);
    } else if (!strcmp(op, "retry")) {
        engine_retry(&s->engine);
        response(req, 1, NULL, NULL);
    } else if (!strcmp(op, "status")) {
        status(s, req);
    } else if (!strcmp(op, "shutdown")) {
        response(req, 1, NULL, NULL);
        s->quitting = 1;
    } else {
        error_response(req, "unknown operation");
    }
    json_free(req);
    return s->fatal ? -1 : 0;
}

/* poll said stdin is readable, so one read cannot block. stdin stays in
 * blocking mode: a stdout sharing its file description (one socket for both
 * directions) must never turn non-blocking. At end of input, every complete
 * request already received is still dispatched. */
static int consume(Server *s)
{
    char bytes[65536], *nl, *record;
    size_t n, used;
    ssize_t got;
    Json *req;
    do
        got = read(STDIN_FILENO, bytes, sizeof(bytes));
    while (got < 0 && errno == EINTR);
    if (got < 0)
        return -1;
    if (got)
        buf_add(&s->input, bytes, (size_t)got);
    used = 0;
    while (!s->quitting && s->input.s &&
           (nl = memchr(s->input.s + used, '\n', s->input.len - used)) != NULL) {
        n = (size_t)(nl - s->input.s) - used;
        record = xmalloc(n + 1);
        memcpy(record, s->input.s + used, n);
        record[n] = 0;
        used += n + 1;
        req = memchr(record, 0, n) ? NULL : json_parse(record);
        free(record);
        if (!req || req->type != J_OBJ) {
            error_response(NULL, "invalid JSON request");
            json_free(req);
            /* An unparsed request has no trustworthy correlation id. Closing
             * the channel rejects every waiter instead of stranding one. */
            return -1;
        } else if (dispatch(s, req) < 0)
            return -1;
    }
    if (used) {
        memmove(s->input.s, s->input.s + used, s->input.len - used);
        s->input.len -= used;
        s->input.s[s->input.len] = 0;
    }
    if (!got)
        s->quitting = 1;
    return 0;
}

static int serve(Server *s)
{
    struct pollfd fd;
    struct sigaction sa;
    int result, k;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    buf_init(&s->input);
    result = 0;
    while (!s->quitting && !s->fatal && !interrupted) {
        if (step(s) < 0) {
            storage_error(s, s->pending ? s->pending : s->gates[0].wait);
            result = -1;
            break;
        }
        fd.fd = STDIN_FILENO;
        fd.events = POLLIN;
        if (poll(&fd, 1, deadline(s)) < 0) {
            if (errno == EINTR)
                continue;
            result = -1;
            break;
        }
        if ((fd.revents & (POLLIN | POLLHUP | POLLERR)) && consume(s) < 0) {
            result = -1;
            break;
        }
    }
    for (k = 0; k < WORKERS; k++) {
        end_call(s, &s->calls[k], 0);
        buf_free(&s->calls[k].messages);
    }
    json_free(s->gates[0].wait);
    free(s->gates[0].key);
    free(s->gates[0].hold);
    json_free(s->pending);
    buf_free(&s->input);
    if (s->fatal)
        result = -1;
    return result;
}

static void usage(void)
{
    fputs("usage: unii prompt\n"
          "       unii serve DIR [--model MODEL] [--provider PROVIDER]\n"
          "                      [--effort LEVEL] [--timeout SECONDS]\n"
          "       unii add DIR KIND [FILE|-]\n"
          "       unii view DIR\n"
          "       unii zoom DIR ID N [PAGE]\n"
          "       unii date DIR ID\n"
          "       unii status DIR\n"
          "       unii log DIR FROM [COUNT]\n", stderr);
}

static char *stdin_text(void)
{
    Buf b;
    char chunk[8192];
    size_t n;
    buf_init(&b);
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) != 0)
        buf_add(&b, chunk, n);
    if (ferror(stdin) || (b.s && memchr(b.s, 0, b.len))) {
        buf_free(&b);
        return NULL;
    }
    if (!b.s)
        return xstrdup("");
    return b.s;
}

static int number(const char *text, unsigned long *n)
{
    Json *j;
    int ok;
    j = json_parse(text);
    ok = json_ulong(j, n);
    json_free(j);
    return ok;
}

int main(int argc, char **argv)
{
    Server s;
    Entry entry;
    Ref leaf;
    char *path, *text;
    const char *command, *assets, *at;
    unsigned long id, n, page;
    Job job;
    int k, result, next, writer;
    memset(&s, 0, sizeof(s));
    if (argc < 2) { usage(); return 2; }
    command = argv[1];
    assets = env_default("UNII_ASSETS", UNII_ASSETS);
    if (!strcmp(command, "prompt")) {
        path = path_join(assets, "prompt.txt");
        text = read_file(path);
        free(path);
        if (!text) { perror("unii: prompt.txt"); return 1; }
        /* prompt.txt is the spec's prompt; C2 rewords one sentence. */
        at = strstr(text, SPEC_PENDING);
        if (TAIL_BYTES && at) {
            fwrite(text, 1, (size_t)(at - text), stdout);
            fputs(TAIL_PENDING, stdout);
            fputs(at + strlen(SPEC_PENDING), stdout);
        } else {
            fputs(text, stdout);
        }
        free(text);
        return 0;
    }
    if (argc < 3) { usage(); return 2; }
    if (!strcmp(command, "serve")) {
        s.model = env_default("UNII_COMPACT_MODEL", "gpt-6-luna");
        s.provider = env_default("UNII_COMPACT_PROVIDER", "openai-codex");
        s.effort = env_default("UNII_COMPACT_EFFORT", "xhigh");
        s.timeout = 300;
        for (k = 3; k < argc; k += 2) {
            if (k + 1 >= argc) { usage(); return 2; }
            if (!strcmp(argv[k], "--model")) s.model = argv[k + 1];
            else if (!strcmp(argv[k], "--provider")) s.provider = argv[k + 1];
            else if (!strcmp(argv[k], "--effort")) s.effort = argv[k + 1];
            else if (!strcmp(argv[k], "--timeout") && number(argv[k + 1], &n) &&
                     n && n <= 86400) s.timeout = (long)n;
            else { usage(); return 2; }
        }
    }
    if (!strcmp(command, "log")) {
        n = ULONG_MAX;
        if (argc < 4 || argc > 5 || !number(argv[3], &id) ||
            (argc == 5 && !number(argv[4], &n))) { usage(); return 2; }
        result = store_log(&s.store, argv[2], id, n, stdout);
        if (result < 0)
            fprintf(stderr, "unii: %s\n", s.store.error);
        store_close(&s.store);
        return result < 0 ? 1 : 0;
    }
    /* Only serve and add write; every other command reads without the lock. */
    writer = !strcmp(command, "serve") || !strcmp(command, "add");
    if ((writer ? store_open(&s.store, argv[2]) : store_open_read(&s.store, argv[2])) < 0) {
        fprintf(stderr, "unii: %s\n", s.store.error);
        store_close(&s.store);
        return 1;
    }
    result = 0;
    if (!strcmp(command, "serve")) {
        if (engine_open(&s.engine, &s.store) < 0) {
            result = -1;
        } else {
            s.engine.built = built;
            s.engine.opaque = &s;
            result = serve(&s);
        }
        engine_close(&s.engine);
    } else if (!strcmp(command, "add")) {
        if (argc < 4 || argc > 5) { usage(); result = -1; goto done; }
        text = argc == 5 && strcmp(argv[4], "-") ? read_file(argv[4]) : stdin_text();
        if (!text) { perror("unii: input"); result = -1; goto done; }
        entry.kind = argv[3];
        entry.text = text;
        entry.images = NULL;
        result = store_append(&s.store, &entry, 1, NULL, NULL);
        free(text);
        if (!result) {
            result = engine_open(&s.engine, &s.store);
            /* Import never fabricates a summary or launches a paid model.
             * Fast paths are built; model work stays queued for pi. */
            while (!result && (next = engine_next(&s.engine, &job)) != 0) {
                if (next < 0)
                    result = -1;
                else
                    engine_fail(&s.engine, job, 0);
            }
            engine_close(&s.engine);
        }
    } else if (!strcmp(command, "view")) {
        text = view_render(&s.store, &s.store.view, s.store.count, 0, 0);
        if (!text) result = -1;
        else { fputs(text, stdout); free(text); }
    } else if (!strcmp(command, "zoom")) {
        page = 0;
        if (argc < 5 || argc > 6 || !number(argv[3], &id) || !number(argv[4], &n) ||
            (argc == 6 && !number(argv[5], &page))) {
            usage(); result = -1; goto done;
        }
        text = store_zoom(&s.store, id, n, page, NULL);
        if (!text) result = -1;
        else { puts(text); free(text); }
    } else if (!strcmp(command, "date")) {
        if (argc != 4 || !number(argv[3], &id)) { usage(); result = -1; goto done; }
        text = store_date(&s.store, id);
        if (!text) result = -1;
        else { puts(text); free(text); }
    } else if (!strcmp(command, "status")) {
        s.engine.store = &s.store;
        leaf.l = 0;
        for (leaf.i = 0; leaf.i < s.store.count && store_node(&s.store, leaf); leaf.i++)
            s.engine.contiguous++;
        status(&s, NULL);
    } else { usage(); result = -1; }
done:
    if (result < 0 && s.store.error[0])
        fprintf(stderr, "unii: %s\n", s.store.error);
    store_close(&s.store);
    return result < 0 ? 1 : 0;
}
