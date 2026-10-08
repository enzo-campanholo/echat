#include "engine.h"
#include "pi.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef UNII_ASSETS
#define UNII_ASSETS "pi"
#endif

typedef struct Worker {
    Pi pi;
    Job job;
    int live, working, attempts;
    char *shortest;
    time_t started;
    unsigned long configuration;
} Worker;
typedef struct Server {
    Store store;
    Engine engine;
    Worker workers[WORKERS];
    PiConfig config;
    char *system, *tools, *provider_tools;
    char *frontend_key;
    Json *cache_wait;
    int frontend_writing;
    Json *pending;
    View preceding;
    unsigned long before;
    unsigned long configuration;
    int quitting, fatal;
    Buf input;
} Server;

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static const char *env_default(const char *key, const char *fallback)
{
    const char *s;
    s = getenv(key);
    return s && *s ? s : fallback;
}

static void response(const Json *req, int ok, const char *field, const char *value)
{
    Buf b;
    const Json *id;
    buf_init(&b);
    id = json_get(req, "id");
    buf_puts(&b, "{\"id\":");
    if (id)
        json_dump(&b, id);
    else
        buf_puts(&b, "null");
    buf_puts(&b, ok ? ",\"ok\":true" : ",\"ok\":false");
    if (field) {
        buf_putc(&b, ',');
        buf_json(&b, field);
        buf_putc(&b, ':');
        buf_json(&b, value ? value : "");
    }
    buf_puts(&b, "}\n");
    if (write_all(STDOUT_FILENO, b.s, b.len) < 0)
        interrupted = 1;
    buf_free(&b);
}

static void error_response(const Json *req, const char *error)
{
    response(req, 0, "error", error);
}

static void image_response(const Json *req, const char *text, const char *images)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":");
    json_dump(&b, json_get(req, "id"));
    buf_puts(&b, ",\"ok\":true,\"text\":");
    buf_json(&b, text);
    if (images) {
        buf_puts(&b, ",\"images\":");
        buf_puts(&b, images);
    }
    buf_puts(&b, "}\n");
    if (write_all(STDOUT_FILENO, b.s, b.len) < 0)
        interrupted = 1;
    buf_free(&b);
}

static char *memory(void *opaque, const char *name, const Json *args)
{
    Server *s;
    unsigned long id, n, page;
    const char *date;
    s = opaque;
    if (!strcmp(name, "date")) {
        if (!json_ulong(json_get(args, "id"), &id))
            return xstrdup("date: id must be a nonnegative integer");
        date = store_date(&s->store, id);
        return xstrdup(date ? date : "date: no such message");
    }
    page = 0;
    n = 1;
    if (!json_ulong(json_get(args, "id"), &id) ||
        (json_get(args, "n") && !json_ulong(json_get(args, "n"), &n)) ||
        (json_get(args, "page") && !json_ulong(json_get(args, "page"), &page)))
        return xstrdup("zoom: id, n and page must be nonnegative integers");
    return store_zoom(&s->store, id, n, page);
}

static int running(Server *s)
{
    int k, n;
    n = 0;
    for (k = 0; k < WORKERS; k++)
        n += s->workers[k].working;
    return n;
}

static void status(Server *s, const Json *req)
{
    Buf b;
    const Json *id;
    buf_init(&b);
    buf_puts(&b, "{\"id\":");
    id = json_get(req, "id");
    if (id) json_dump(&b, id); else buf_puts(&b, "null");
    buf_puts(&b, ",\"ok\":true,\"messages\":");
    buf_ulong(&b, s->store.count);
    buf_puts(&b, ",\"summarized\":");
    buf_ulong(&b, s->engine.contiguous);
    buf_puts(&b, ",\"view_bytes\":");
    buf_ulong(&b, (unsigned long)view_measure(&s->store, &s->store.view));
    buf_puts(&b, ",\"lines\":");
    buf_ulong(&b, (unsigned long)s->store.view.n);
    buf_puts(&b, ",\"workers\":");
    buf_ulong(&b, (unsigned long)running(s));
    buf_puts(&b, ",\"failed\":");
    buf_ulong(&b, (unsigned long)(s->engine.failed.n - s->engine.failed.head));
    buf_puts(&b, "}\n");
    if (write_all(STDOUT_FILENO, b.s, b.len) < 0)
        interrupted = 1;
    buf_free(&b);
}

static int append(Server *s, const char *kind, const char *text, const Json *images)
{
    if (!store_append_images(&s->store, kind, text, images, NULL, NULL) ||
        engine_sync(&s->engine) < 0)
        return -1;
    return 0;
}

static void cache_settle(Server *s)
{
    Worker *w;
    int k;
    if (!s->cache_wait)
        return;
    for (k = 0; k < WORKERS; k++) {
        w = &s->workers[k];
        if (w->working && !w->pi.failed && !w->pi.first_response &&
            !w->pi.prefix_waiting && w->pi.prefix_key &&
            !strcmp(w->pi.prefix_key, s->frontend_key))
            return;
    }
    response(s->cache_wait, 1, NULL, NULL);
    json_free(s->cache_wait);
    s->cache_wait = NULL;
    s->frontend_writing = 1;
}

static int settle_pending(Server *s)
{
    Json *req;
    const char *op;
    char *view;
    if (!s->pending)
        return 0;
    req = s->pending;
    op = json_str(json_get(req, "op"));
    if (!engine_caught_up(&s->engine, s->before)) {
        if (!running(s) && s->engine.failed.n != s->engine.failed.head) {
            /* Do not call a turn on incomplete memory. The next input is an
             * explicit retry opportunity, not a fabricated successful turn. */
            error_response(req, !strcmp(op, "turn") ?
                "compaction failed; input is saved; send a message to retry" :
                "compaction failed; send a message to retry");
            json_free(req);
            s->pending = NULL;
            view_free(&s->preceding);
        }
        return 0;
    }
    if (!strcmp(op, "flush")) {
        if (running(s) || s->engine.ready.head != s->engine.ready.n ||
            s->engine.leaves.head != s->engine.leaves.n)
            return 0;
        if (s->engine.failed.head != s->engine.failed.n) {
            error_response(req, "some compactions failed; send a message to retry");
            json_free(req);
            s->pending = NULL;
            return 0;
        }
    }
    if (!strcmp(op, "flush")) {
        response(req, 1, NULL, NULL);
    } else {
        view = view_render(&s->store, &s->preceding, s->before, 1);
        response(req, 1, "view", view);
        free(view);
    }
    json_free(req);
    s->pending = NULL;
    view_free(&s->preceding);
    return 0;
}

static int dispatch(Server *s, Json *req)
{
    const char *op, *text, *kind, *system;
    char *images;
    Json *j;
    Buf b;
    unsigned long first, id, n, page;
    int k;
    op = json_str(json_get(req, "op"));
    if (!op) {
        error_response(req, "missing operation");
    } else if (!strcmp(op, "configure")) {
        system = json_str(json_get(req, "system"));
        if (!system || !*system) {
            error_response(req, "configure needs a nonempty system prompt");
        } else if (s->system && strcmp(s->system, system) && running(s)) {
            error_response(req, "cannot change the system while compactions are running");
        } else {
            s->configuration++;
            for (k = 0; k < WORKERS; k++) {
                if (s->workers[k].live && !s->workers[k].working) {
                    pi_stop(&s->workers[k].pi);
                    s->workers[k].live = 0;
                }
            }
            free(s->system);
            s->system = xstrdup(system);
            s->config.system = s->system;
            j = json_get(req, "tools");
            if (j && j->type == J_ARR) {
                buf_init(&b);
                json_dump(&b, j);
                free(s->tools);
                s->tools = b.s;
                s->config.tools = s->tools;
            }
            j = json_get(req, "provider_tools");
            if (j && j->type == J_ARR) {
                buf_init(&b);
                json_dump(&b, j);
                free(s->provider_tools);
                s->provider_tools = b.s;
                s->config.provider_tools = s->provider_tools;
            }
            response(req, 1, NULL, NULL);
        }
    } else if (!strcmp(op, "turn") || !strcmp(op, "flush")) {
        text = json_str(json_get(req, "text"));
        if (!s->system) {
            error_response(req, "configure the backend before starting a turn");
        } else if (s->pending) {
            error_response(req, "another turn is waiting for memory");
        } else if (!strcmp(op, "turn") && !text) {
            error_response(req, "turn needs text");
        } else {
            /* Failed calls retry only at an incoming message. No timed retry. */
            engine_retry(&s->engine);
            s->before = s->store.count;
            /* Capture the preceding view before recording input. Its missing
             * nodes may finish later; its boundary must never move. */
            if (!strcmp(op, "turn")) {
                view_copy(&s->preceding, &s->store.view);
                if (append(s, "user", text, json_get(req, "images")) < 0) {
                    error_response(req, s->store.error);
                    s->fatal = 1;
                    view_free(&s->preceding);
                    json_free(req);
                    return -1;
                }
            }
            s->pending = req;
            return settle_pending(s);
        }
    } else if (!strcmp(op, "cache_wait")) {
        text = json_str(json_get(req, "key"));
        if (!text || !*text || s->cache_wait) {
            error_response(req, "invalid or duplicate cache wait");
        } else {
            free(s->frontend_key);
            s->frontend_key = xstrdup(text);
            s->frontend_writing = 0;
            s->cache_wait = req;
            cache_settle(s);
            return 0;
        }
    } else if (!strcmp(op, "cache_started")) {
        text = json_str(json_get(req, "key"));
        if (text && s->frontend_key && !strcmp(text, s->frontend_key))
            s->frontend_writing = 0;
        response(req, 1, NULL, NULL);
    } else if (!strcmp(op, "append") || !strcmp(op, "steer")) {
        text = json_str(json_get(req, "text"));
        kind = !strcmp(op, "steer") ? "user" : json_str(json_get(req, "kind"));
        first = s->store.count;
        if (!text || !kind) {
            error_response(req, "append needs kind and text");
        } else if (append(s, kind, text, json_get(req, "images")) < 0) {
            error_response(req, s->store.error);
            s->fatal = 1;
        } else {
            buf_init(&b);
            buf_ulong(&b, first);
            response(req, 1, "message_id", b.s);
            buf_free(&b);
        }
    } else if (!strcmp(op, "zoom") || !strcmp(op, "date")) {
        j = json_get(req, "args");
        text = memory(s, op, j ? j : req);
        if (!text) error_response(req, s->store.error);
        else {
            images = NULL;
            n = 1;
            page = 0;
            if (!strcmp(op, "zoom") && j &&
                json_ulong(json_get(j, "id"), &id) &&
                (!json_get(j, "n") || json_ulong(json_get(j, "n"), &n)) && n == 1 &&
                (!json_get(j, "page") || json_ulong(json_get(j, "page"), &page)) && !page)
                images = store_images(&s->store, id);
            image_response(req, text, images);
            free(images);
            free((char *)text);
        }
    } else if (!strcmp(op, "status")) {
        status(s, req);
    } else if (!strcmp(op, "shutdown")) {
        response(req, 1, NULL, NULL);
        s->quitting = 1;
    } else {
        error_response(req, "unknown operation");
    }
    json_free(req);
    return 0;
}

static int consume(Server *s)
{
    char bytes[8192], *nl, *record;
    size_t n, used;
    ssize_t got;
    Json *req;
    for (;;) {
        got = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (got > 0)
            buf_add(&s->input, bytes, (size_t)got);
        else if (got == 0) { s->quitting = 1; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else return -1;
    }
    used = 0;
    while (s->input.s && (nl = memchr(s->input.s + used, '\n', s->input.len - used)) != NULL) {
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
    return 0;
}

static void failed_job(Server *s, Worker *w, const char *why)
{
    fprintf(stderr, "unii: compaction %lu+%lu failed: %s\n",
        ref_start(w->job.ref), ref_span(w->job.ref), why);
    engine_fail(&s->engine, w->job);
    free(w->shortest);
    w->shortest = NULL;
    w->working = 0;
    if (w->live) {
        pi_stop(&w->pi);
        w->live = 0;
    }
}

static int workers_step(Server *s)
{
    Worker *w, *other;
    PiCallbacks callbacks;
    Buf correction;
    char *task, *view, *images;
    const char *line;
    size_t length, cut;
    int k, j, next, blocked;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.opaque = s;
    callbacks.memory = memory;
    for (k = 0; k < WORKERS; k++) {
        w = &s->workers[k];
        if (!w->live || !w->working)
            continue;
        pi_process(&w->pi, &callbacks);
        if (w->pi.prefix_waiting) {
            blocked = s->frontend_writing && s->frontend_key &&
                !strcmp(s->frontend_key, w->pi.prefix_key);
            for (j = 0; j < WORKERS; j++) {
                other = &s->workers[j];
                if (j != k && other->working && other->pi.prefix_key &&
                    !strcmp(other->pi.prefix_key, w->pi.prefix_key) &&
                    !other->pi.prefix_waiting && !other->pi.first_response &&
                    !other->pi.failed)
                    blocked = 1;
            }
            if (!blocked)
                pi_release_prefix(&w->pi);
        }
        if (w->pi.failed) {
            failed_job(s, w, w->pi.error.s ? w->pi.error.s : "Pi failed");
            continue;
        }
        if (time(NULL) - w->started > 180) {
            failed_job(s, w, "call timed out after 180 seconds");
            continue;
        }
        if (!w->pi.done)
            continue;
        line = w->pi.result.s;
        if (!line || !*line || !utf8_valid(line)) {
            failed_job(s, w, "empty or invalid UTF-8 summary");
            continue;
        }
        length = strlen(line);
        if (!w->shortest || length < strlen(w->shortest)) {
            free(w->shortest);
            w->shortest = xstrdup(line);
        }
        w->attempts++;
        if (length <= NODE_BYTES || w->attempts >= 5) {
            if (engine_done(&s->engine, w->job, w->shortest) < 0)
                return -1;
            free(w->shortest);
            w->shortest = NULL;
            w->working = 0;
        } else {
            buf_init(&correction);
            buf_puts(&correction, "Too long: your line is ");
            buf_ulong(&correction, (unsigned long)length);
            buf_puts(&correction, " bytes, over the 512-byte limit. Write\nthe whole line again for the same <input>, cutting just enough of the\nleast valuable items to fit before this cut:\n");
            cut = utf8_prefix(line, NODE_BYTES);
            buf_add(&correction, line, cut);
            buf_puts(&correction, "| \342\206\220 LIMIT");
            if (pi_prompt(&w->pi, NULL, correction.s, 0) < 0)
                failed_job(s, w, "cannot send length correction");
            buf_free(&correction);
            w->started = time(NULL);
        }
    }
    cache_settle(s);
    if (!s->system)
        return 0;
    for (k = 0; k < WORKERS; k++) {
        w = &s->workers[k];
        if (w->working)
            continue;
        next = engine_next(&s->engine, &w->job);
        if (next < 0)
            return -1;
        if (!next)
            break;
        w->working = 1;
        w->attempts = 0;
        w->started = time(NULL);
        if (w->live && w->configuration != s->configuration) {
            pi_stop(&w->pi);
            w->live = 0;
        }
        if (!w->live) {
            if (pi_start(&w->pi, &s->config) < 0) {
                failed_job(s, w, "cannot start pi; check UNII_PI and the bridge path");
                continue;
            }
            w->live = 1;
            w->configuration = s->configuration;
        }
        task = engine_task(&s->engine, w->job);
        view = engine_context(&s->engine, w->job);
        images = w->job.ref.l ? NULL : store_images(&s->store, w->job.ref.i);
        if (!task || !view || (!w->job.ref.l &&
            s->store.messages[w->job.ref.i].has_images && !images) ||
            pi_prompt_images(&w->pi, view, task, images, 1) < 0) {
            failed_job(s, w, "cannot start compaction");
        }
        free(task);
        free(view);
        free(images);
    }
    return settle_pending(s);
}

static int serve(Server *s)
{
    struct pollfd fds[WORKERS * 3 + 1];
    int count, k, flags, result;
    Worker *w;
    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    buf_init(&s->input);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    result = 0;
    while (!s->quitting && !s->fatal && !interrupted) {
        if (workers_step(s) < 0) { result = -1; break; }
        count = 0;
        fds[count].fd = STDIN_FILENO;
        fds[count].events = POLLIN;
        count++;
        for (k = 0; k < WORKERS; k++) {
            w = &s->workers[k];
            if (!w->live || !w->working)
                continue;
            fds[count].fd = w->pi.fd;
            fds[count++].events = POLLIN;
            fds[count].fd = w->pi.toolfd;
            fds[count++].events = POLLIN |
                (w->pi.toolout.len > w->pi.toolout_pos ? POLLOUT : 0);
            if (w->pi.out.len > w->pi.out_pos) {
                fds[count].fd = w->pi.writefd;
                fds[count++].events = POLLOUT;
            }
        }
        if (poll(fds, (nfds_t)count, 1000) < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (consume(s) < 0) { result = -1; break; }
        }
    }
    for (k = 0; k < WORKERS; k++) {
        if (s->workers[k].live)
            pi_stop(&s->workers[k].pi);
        free(s->workers[k].shortest);
    }
    json_free(s->pending);
    view_free(&s->preceding);
    json_free(s->cache_wait);
    buf_free(&s->input);
    free(s->system);
    free(s->tools);
    free(s->provider_tools);
    free(s->frontend_key);
    if (s->fatal)
        result = -1;
    return result;
}

static void usage(void)
{
    fputs("usage: unii prompt\n"
          "       unii serve DIR [--pi EXE] [--bridge FILE] [--model MODEL]\n"
          "                      [--provider PROVIDER] [--effort LEVEL]\n"
          "       unii add DIR KIND [FILE|-]\n"
          "       unii view DIR\n"
          "       unii zoom DIR ID N [PAGE]\n"
          "       unii date DIR ID\n"
          "       unii status DIR\n", stderr);
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
    char *path, *text;
    const char *command, *assets, *date;
    unsigned long id, n, page;
    Job job;
    int k, result, next;
    memset(&s, 0, sizeof(s));
    if (argc < 2) { usage(); return 2; }
    command = argv[1];
    assets = env_default("UNII_ASSETS", UNII_ASSETS);
    if (!strcmp(command, "prompt")) {
        path = path_join(assets, "prompt.txt");
        text = read_file(path);
        free(path);
        if (!text) { perror("unii: prompt.txt"); return 1; }
        fputs(text, stdout);
        free(text);
        return 0;
    }
    if (argc < 3) { usage(); return 2; }
    if (!store_open(&s.store, argv[2])) {
        fprintf(stderr, "unii: %s\n", s.store.error);
        store_close(&s.store);
        return 1;
    }
    result = 0;
    if (!strcmp(command, "serve")) {
        if (engine_open(&s.engine, &s.store) < 0) {
            engine_close(&s.engine);
            result = -1;
            goto done;
        }
        path = path_join(assets, "bridge.mjs");
        s.config.command = env_default("UNII_PI", "pi");
        s.config.extension = path;
        s.config.model = env_default("UNII_COMPACT_MODEL", "claude-haiku-4-5");
        s.config.provider = env_default("UNII_COMPACT_PROVIDER", "anthropic");
        s.config.effort = env_default("UNII_COMPACT_EFFORT", "xhigh");
        s.config.compaction = 1;
        s.config.cache_gate = 1;
        for (k = 3; k < argc; k += 2) {
            if (k + 1 >= argc) { result = -1; break; }
            if (!strcmp(argv[k], "--pi")) s.config.command = argv[k + 1];
            else if (!strcmp(argv[k], "--bridge")) s.config.extension = argv[k + 1];
            else if (!strcmp(argv[k], "--model")) s.config.model = argv[k + 1];
            else if (!strcmp(argv[k], "--provider")) s.config.provider = argv[k + 1];
            else if (!strcmp(argv[k], "--effort")) s.config.effort = argv[k + 1];
            else { result = -1; break; }
        }
        if (!result) result = serve(&s); else usage();
        free(path);
        engine_close(&s.engine);
    } else if (!strcmp(command, "add")) {
        if (argc < 4 || argc > 5) { usage(); result = -1; goto done; }
        text = argc == 5 && strcmp(argv[4], "-") ? read_file(argv[4]) : stdin_text();
        if (!text) { perror("unii: input"); result = -1; goto done; }
        result = store_append(&s.store, argv[3], text, NULL, NULL) ? 0 : -1;
        free(text);
        if (!result) {
            result = engine_open(&s.engine, &s.store);
            if (result < 0) {
                engine_close(&s.engine);
                goto done;
            }
            /* Import never fabricates a summary or launches a paid model.
             * Fast paths are built; model work stays queued for pi. */
            while ((next = engine_next(&s.engine, &job)) == 1)
                engine_fail(&s.engine, job);
            if (next < 0) result = -1;
            engine_close(&s.engine);
        }
    } else if (!strcmp(command, "view")) {
        text = view_render(&s.store, &s.store.view, s.store.count, 0);
        fputs(text, stdout);
        free(text);
    } else if (!strcmp(command, "zoom")) {
        page = 0;
        if (argc < 5 || argc > 6 || !number(argv[3], &id) || !number(argv[4], &n) ||
            (argc == 6 && !number(argv[5], &page))) {
            usage(); result = -1; goto done;
        }
        text = store_zoom(&s.store, id, n, page);
        if (!text) result = -1;
        else { puts(text); free(text); }
    } else if (!strcmp(command, "date")) {
        if (argc != 4 || !number(argv[3], &id)) { usage(); result = -1; goto done; }
        date = store_date(&s.store, id);
        if (!date) { strcpy(s.store.error, "no such message"); result = -1; }
        else puts(date);
    } else if (!strcmp(command, "status")) {
        Ref leaf;
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
