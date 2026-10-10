/* The test plays the host: it answers the engine's call events itself. */
#include "base.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char *big; /* 40,000 bytes: two messages, more than TAIL_BYTES */

enum {
    ANSWER_CORRECTED, /* 600 bytes, then a short line after "Too long:" */
    ANSWER_500,       /* 500 bytes at once */
    ANSWER_HUGE,      /* 40,000 bytes every time */
    ANSWER_NONE,      /* never answer: the call must time out */
    ANSWER_MANUAL     /* queue calls for the test to answer */
};

typedef struct Client {
    FILE *in, *out;
    pid_t pid;
    unsigned long serial, results;
    int policy, fail_once;
    const char *fail_merges; /* marker file: fail merges while it exists */
    unsigned long calls, errors, cancels, last_call, queued[16];
    unsigned long appends, append_next, builts, progress, waiting;
    size_t nqueued, longest;
    char *error_text;
} Client;

static Client start_binary(const char *dir, const char *binary, const char *timeout)
{
    int in[2], out[2], quiet;
    Client c;
    memset(&c, 0, sizeof(c));
    c.waiting = (unsigned long)-1;
    assert(!pipe(in) && !pipe(out));
    c.pid = fork();
    assert(c.pid >= 0);
    if (!c.pid) {
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        if (getenv("UNII_TEST_QUIET")) {
            quiet = open("/dev/null", O_WRONLY);
            assert(quiet >= 0 && dup2(quiet, STDERR_FILENO) >= 0);
            close(quiet);
        }
        if (timeout)
            execl(binary, "unii", "serve", dir, "--provider", "test-provider",
                  "--model", "test-model", "--timeout", timeout, (char *)NULL);
        else
            execl(binary, "unii", "serve", dir, "--provider", "test-provider",
                  "--model", "test-model", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    c.in = fdopen(out[0], "r");
    c.out = fdopen(in[1], "w");
    assert(c.in && c.out);
    return c;
}

static Client start(const char *dir)
{
    return start_binary(dir, "./unii", NULL);
}

static void put(Client *c, const char *op, const char *kind, const char *text)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, ++c->serial);
    buf_puts(&b, ",\"op\":"); buf_json(&b, op);
    if (kind) { buf_puts(&b, ",\"kind\":"); buf_json(&b, kind); }
    if (text) { buf_puts(&b, ",\"text\":"); buf_json(&b, text); }
    buf_puts(&b, "}\n");
    assert(fputs(b.s, c->out) >= 0);
    buf_free(&b);
}

/* Results use string ids ("r1", ...) so their acknowledgements are skipped. */
static void result(Client *c, unsigned long call, const char *text, const char *error)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":\"r"); buf_ulong(&b, ++c->results);
    buf_puts(&b, "\",\"op\":\"result\",\"call\":"); buf_ulong(&b, call);
    if (error) { buf_puts(&b, ",\"error\":"); buf_json(&b, error); }
    else { buf_puts(&b, ",\"text\":"); buf_json(&b, text); }
    buf_puts(&b, "}\n");
    assert(fputs(b.s, c->out) >= 0 && !fflush(c->out));
    buf_free(&b);
}

/* Check the call's shape the way a host would see it, then answer it. */
static void on_call(Client *c, const Json *j)
{
    const Json *m, *first, *last;
    const char *text, *end;
    unsigned long id;
    size_t count, k;
    Buf answer;
    assert(json_ulong(json_get(j, "call"), &id) && id > c->last_call);
    c->last_call = id;
    c->calls++;
    assert(!strcmp(json_str(json_get(j, "provider")), "test-provider"));
    assert(!strcmp(json_str(json_get(j, "model")), "test-model"));
    assert(!strcmp(json_str(json_get(j, "effort")), "xhigh"));
    m = json_get(j, "messages");
    assert(m && m->type == J_ARR && m->child);
    first = m->child;
    count = 0;
    for (last = first; last; last = last->next) {
        assert(!strcmp(json_str(json_get(last, "role")), count % 2 ? "assistant" : "user"));
        count++;
        if (!last->next)
            break;
    }
    assert(count % 2 == 1 && count <= 9);
    text = json_str(json_get(first, "text"));
    assert(!strncmp(text, "<chat>\n", 7));
    end = strstr(text, "</chat>\n");
    assert(end && (size_t)(end - text) + 8 <= 32000);
    assert(strstr(text, "Compaction:") && strstr(text, "<input>\n"));
    assert(!strstr(text, "(not summarized yet:"));
    if (count > 1)
        assert(!strncmp(json_str(json_get(last, "text")), "Too long: ", 10));
    if (c->policy == ANSWER_NONE)
        return;
    if (c->policy == ANSWER_MANUAL) {
        assert(c->nqueued < 16);
        c->queued[c->nqueued++] = id;
        return;
    }
    if (c->fail_once || (c->fail_merges && access(c->fail_merges, F_OK) == 0 &&
                         strstr(text, "Compaction: merge lines"))) {
        c->fail_once = 0;
        result(c, id, NULL, "test transient failure");
        return;
    }
    buf_init(&answer);
    if (c->policy == ANSWER_500)
        for (k = 0; k < 500; k++) buf_putc(&answer, 's');
    else if (c->policy == ANSWER_HUGE)
        for (k = 0; k < 40000; k++) buf_putc(&answer, 'h');
    else if (count > 1)
        buf_puts(&answer, "user: deterministic summary preserving the test's correction and result.");
    else
        for (k = 0; k < 600; k++) buf_putc(&answer, 'x');
    result(c, id, answer.s, NULL);
    buf_free(&answer);
}

/* Next response to one of our numbered requests; events are handled here. */
static Json *receive(Client *c)
{
    Buf input;
    Json *j;
    const char *event;
    unsigned long n, first;
    for (;;) {
        buf_init(&input);
        assert(read_line(c->in, &input) == 1);
        j = json_parse(input.s);
        assert(j);
        buf_free(&input);
        event = json_str(json_get(j, "event"));
        assert(!event || !json_get(j, "id")); /* events never look like responses */
        if (!event && json_get(j, "id") && json_get(j, "id")->type != J_STR)
            return j;
        if (!event) {
            assert(!strcmp(json_get(j, "ok")->s, "true"));
        } else if (!strcmp(event, "call")) {
            on_call(c, j);
        } else if (!strcmp(event, "error")) {
            c->errors++;
            assert(!json_get(j, "id") && json_get(j, "l") && json_get(j, "i"));
            assert(json_str(json_get(j, "provider")) && json_str(json_get(j, "model")));
            free(c->error_text);
            c->error_text = xstrdup(json_str(json_get(j, "text")));
        } else if (!strcmp(event, "append")) {
            assert(json_ulong(json_get(j, "first"), &first));
            assert(!c->appends || first == c->append_next); /* contiguous ids */
            assert(json_ulong(json_get(j, "count"), &n) && n);
            assert(json_str(json_get(j, "kind")));
            c->appends += n;
            c->append_next = first + n;
        } else if (!strcmp(event, "built")) {
            assert(json_get(j, "l") && json_get(j, "i"));
            c->builts++;
        } else if (!strcmp(event, "progress")) {
            assert(json_ulong(json_get(j, "waiting"), &n));
            c->waiting = n;
            c->progress++;
        } else if (!strcmp(event, "cancel")) {
            c->cancels++;
        }
        json_free(j);
    }
}

static Json *request(Client *c, const char *op, const char *kind, const char *text)
{
    put(c, op, kind, text);
    assert(!fflush(c->out));
    return receive(c);
}

static void ok(Json *j)
{
    Json *flag;
    flag = json_get(j, "ok");
    if (!flag || !flag->s || strcmp(flag->s, "true")) {
        fprintf(stderr, "server error: %s\n", json_str(json_get(j, "error")));
        assert(0);
    }
    json_free(j);
}

static void hello(Client *c)
{
    Json *j;
    unsigned long n;
    j = request(c, "hello", NULL, NULL);
    assert(json_ulong(json_get(j, "protocol"), &n) && n == 1);
    assert(json_ulong(json_get(j, "messages"), &n));
    assert(!strcmp(json_str(json_get(j, "model")), "test-model"));
    ok(j);
}

static void stop(Client *c)
{
    int status;
    c->policy = ANSWER_NONE; /* calls still in flight die with the engine */
    ok(request(c, "shutdown", NULL, NULL));
    fclose(c->out); fclose(c->in);
    assert(waitpid(c->pid, &status, 0) == c->pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    free(c->error_text);
}

/* CLI readers run beside the engine, which holds the writer lock (B3/A4). */
static void read_beside(const char *dir)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "./unii status '");
    buf_puts(&b, dir);
    buf_puts(&b, "' | grep -q '\"messages\":20,' && test \"$(./unii log '");
    buf_puts(&b, dir);
    buf_puts(&b, "' 18 5 | wc -l)\" -eq 2 && ./unii view '");
    buf_puts(&b, dir);
    buf_puts(&b, "' | grep -q '^0+1|user: first user message$'");
    assert(system(b.s) == 0);
    buf_free(&b);
}

static void failed_input(const char *root, int events)
{
    Client c;
    char *dir;
    Json *j;
    unsigned long count;
    const char *view;
    dir = path_join(root, events ? "failed-events" : "failed-input");
    if (!events)
        assert(!setenv("UNII_TEST_QUIET", "1", 1));
    c = start(dir);
    assert(!unsetenv("UNII_TEST_QUIET"));
    c.fail_once = 1;
    ok(request(&c, "append", "note", big)); /* two messages, over TAIL_BYTES */
    /* Protocol 0 configure still enables calls; events follow its flag. */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"configure\",\"system\":\"ignored\",\"events\":%s}\n",
                   ++c.serial, events ? "true" : "false") > 0);
    put(&c, "turn", NULL, "INPUT_SAVED_DESPITE_SUMMARY_FAILURE");
    assert(!fflush(c.out));
    ok(receive(&c));
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    assert(c.errors == (events ? 1U : 0U));
    if (events)
        assert(!strcmp(c.error_text, "test transient failure") &&
               c.progress >= 1 && c.waiting == 2);
    j = request(&c, "flush", NULL, NULL);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 3);
    assert(json_ulong(json_get(j, "failed"), &count) && count == 1);
    ok(j);
    j = request(&c, "turn", NULL, "retry after failure");
    view = json_str(json_get(j, "view"));
    assert(view && strstr(view, "INPUT_SAVED_DESPITE_SUMMARY_FAILURE"));
    assert(!strstr(view, "retry after failure") && !strstr(view, "not summarized yet"));
    ok(j);
    stop(&c);
    free(dir);
}

/* History pages by id within a byte budget; images only on request (B4). */
static void history(const char *root, const char *text)
{
    Client c;
    char *dir;
    Json *j;
    const Json *r;
    unsigned long n;
    int k;
    dir = path_join(root, "history");
    c = start(dir);
    hello(&c);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"append\",\"kind\":\"user\",\"text\":\"look\","
                   "\"images\":[{\"type\":\"image\",\"data\":\"AA==\",\"mimeType\":\"image/png\"}]}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    ok(receive(&c));
    for (k = 0; k < 5; k++)
        ok(request(&c, "append", "note", text));
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":0,\"count\":2}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    r = json_get(j, "records")->child;
    assert(json_ulong(json_get(r, "i"), &n) && n == 0);
    assert(!strcmp(json_str(json_get(r, "kind")), "user") && !strcmp(json_str(json_get(r, "text")), "look"));
    assert(json_ulong(json_get(r, "size"), &n) && n == 4 && json_str(json_get(r, "date")));
    assert(!strcmp(json_get(r, "has_images")->s, "true") && !json_get(r, "images"));
    r = r->next;
    assert(json_ulong(json_get(r, "i"), &n) && n == 1 && !strcmp(json_get(r, "has_images")->s, "false"));
    assert(!r->next && json_ulong(json_get(j, "next"), &n) && n == 2);
    ok(j);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":0,\"count\":1,\"images\":true}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    r = json_get(j, "records")->child;
    assert(json_get(r, "images") && json_get(r, "images")->type == J_ARR);
    ok(j);
    /* The first record always comes; later ones stop at the budget. */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":1,\"count\":9,\"bytes\":1500}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(json_ulong(json_get(j, "next"), &n) && n == 3);
    ok(j);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":4,\"count\":9}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(json_ulong(json_get(j, "next"), &n) && n == 6);
    ok(j);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":99}\n", ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(!json_get(j, "records")->child && json_ulong(json_get(j, "next"), &n) && n == 99);
    ok(j);
    assert(c.appends == 6);
    stop(&c);
    free(dir);
}

/* Spec change C2: a turn need not wait for the summaries of what precedes it;
 * those messages come whole in "recent" when they fit in TAIL_BYTES. */
static void recent_tail(const char *root, const char *text)
{
    Client c;
    char *dir;
    Json *j;
    Buf expected;
    const char *view;
    size_t k;
    dir = path_join(root, "recent");
    c = start(dir);
    c.policy = ANSWER_MANUAL;
    ok(request(&c, "append", "note", text));
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"append\",\"kind\":\"user\",\"text\":\"see\\nthis\","
                   "\"images\":[{\"type\":\"image\",\"data\":\"AA==\",\"mimeType\":\"image/png\"}]}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    ok(receive(&c));
    hello(&c);
    j = request(&c, "turn", NULL, "NOW");
    assert(!strcmp(json_str(json_get(j, "view")), "<chat>\n</chat>\n"));
    buf_init(&expected);
    buf_puts(&expected, "<recent>\n[message 0 kind=note]\n");
    buf_puts(&expected, text);
    buf_puts(&expected, "\n[message 1 kind=user]\nsee\nthis\n(has images: zoom(1,1))\n</recent>\n");
    assert(!strcmp(json_str(json_get(j, "recent")), expected.s));
    ok(j);
    buf_free(&expected);
    /* The summaries finish later; the next turn sees them in the view. */
    assert(c.nqueued == 2);
    c.policy = ANSWER_CORRECTED;
    for (k = 0; k < c.nqueued; k++)
        result(&c, c.queued[k], "note: summarized", NULL);
    ok(request(&c, "flush", NULL, NULL));
    j = request(&c, "turn", NULL, "LATER");
    view = json_str(json_get(j, "view"));
    assert(view && strstr(view, "0+1|note: summarized") && !json_get(j, "recent"));
    ok(j);
    stop(&c);
    free(dir);
}

/* A message typed during a flush supersedes the flush instead of being
 * refused; a turn arriving while another turn waits is still refused. */
static void flush_then_turn(const char *root)
{
    Client c;
    char *dir;
    Json *j;
    unsigned long flush, got, count;
    size_t k;
    dir = path_join(root, "flush-turn");
    c = start(dir);
    c.policy = ANSWER_MANUAL;
    ok(request(&c, "append", "note", big)); /* two messages, over TAIL_BYTES */
    hello(&c);
    put(&c, "flush", NULL, NULL);
    flush = c.serial;
    put(&c, "turn", NULL, "TYPED_DURING_FLUSH");
    put(&c, "turn", NULL, "REFUSED_WHILE_A_TURN_WAITS");
    assert(!fflush(c.out));
    j = receive(&c);
    assert(json_ulong(json_get(j, "id"), &got) && got == flush);
    assert(!strcmp(json_str(json_get(j, "error")), "flush superseded by a turn"));
    json_free(j);
    j = receive(&c);
    assert(json_ulong(json_get(j, "id"), &got) && got == flush + 2);
    assert(!strcmp(json_str(json_get(j, "error")), "another turn is waiting for memory"));
    json_free(j);
    assert(c.nqueued == 2);
    c.policy = ANSWER_CORRECTED;
    for (k = 0; k < c.nqueued; k++)
        result(&c, c.queued[k], "note: summarized", NULL);
    j = receive(&c);
    assert(json_ulong(json_get(j, "id"), &got) && got == flush + 1);
    assert(strstr(json_str(json_get(j, "view")), "note: summarized") && !json_get(j, "recent"));
    assert(!strstr(json_str(json_get(j, "view")), "TYPED_DURING_FLUSH"));
    ok(j);
    ok(request(&c, "flush", NULL, NULL));
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 3);
    ok(j);
    stop(&c);
    free(dir);
}

/* One socket as both stdin and stdout (inetd, socat EXEC). Responses the
 * host has not read yet must block the engine, never end it. */
static void shared_socket(const char *root)
{
    Client c;
    char *dir, *note;
    Json *j;
    unsigned long n, first;
    int fds[2], k;
    dir = path_join(root, "shared-socket");
    memset(&c, 0, sizeof(c));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    c.pid = fork();
    assert(c.pid >= 0);
    if (!c.pid) {
        assert(dup2(fds[1], 0) >= 0 && dup2(fds[1], 1) >= 0);
        close(fds[0]); close(fds[1]);
        execl("./unii", "unii", "serve", dir, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    c.in = fdopen(fds[0], "r");
    c.out = fdopen(dup(fds[0]), "w");
    assert(c.in && c.out);
    note = xmalloc(30001);
    memset(note, 'S', 30000); note[30000] = 0;
    for (k = 0; k < 10; k++)
        ok(request(&c, "append", "note", note));
    /* 60 answers of 300 KB each, far more than the socket buffers. */
    first = c.serial + 1;
    for (k = 0; k < 60; k++)
        assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"messages\",\"from\":0,\"count\":10}\n",
                       ++c.serial) > 0);
    assert(!fflush(c.out));
    sleep(1);
    for (k = 0; k < 60; k++) {
        j = receive(&c);
        assert(json_ulong(json_get(j, "id"), &n) && n == first + (unsigned long)k);
        assert(json_ulong(json_get(j, "next"), &n) && n == 10);
        ok(j);
    }
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &n) && n == 10);
    ok(j);
    stop(&c);
    free(note);
    free(dir);
}

/* A new chat's own entry in its parent directory is synced at open. */
static void parent_sync(const char *root)
{
    Client c;
    char *parent, *dir;
    Buf line;
    int status;
    parent = path_join(root, "parent");
    dir = path_join(parent, "chat");
    assert(!setenv("UNII_TEST_FSYNC_PATH", parent, 1) && !setenv("UNII_TEST_QUIET", "1", 1));
    c = start_binary(dir, "./tests/fsync_test", NULL);
    assert(!unsetenv("UNII_TEST_FSYNC_PATH") && !unsetenv("UNII_TEST_QUIET"));
    assert(!fclose(c.out));
    buf_init(&line);
    assert(read_line(c.in, &line) == 0);
    buf_free(&line);
    assert(!fclose(c.in));
    assert(waitpid(c.pid, &status, 0) == c.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    c = start(dir);
    ok(request(&c, "status", NULL, NULL));
    stop(&c);
    free(parent);
    free(dir);
}

/* A retry op restarts failed work without inventing a message (B5). */
static void explicit_retry(const char *root, const char *text)
{
    Client c;
    char *dir;
    Json *j;
    unsigned long count;
    dir = path_join(root, "retry");
    c = start(dir);
    c.fail_once = 1;
    ok(request(&c, "append", "note", text));
    hello(&c);
    j = request(&c, "flush", NULL, NULL);
    assert(json_str(json_get(j, "error")) && c.errors == 1);
    json_free(j);
    ok(request(&c, "retry", NULL, NULL));
    ok(request(&c, "flush", NULL, NULL));
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 1);
    assert(json_ulong(json_get(j, "failed"), &count) && count == 0);
    ok(j);
    stop(&c);
    free(dir);
}

/* A call that never answers is cancelled after --timeout (monotonic clock). */
static void timed_out_call(const char *root)
{
    Client c;
    char *dir;
    Json *j;
    unsigned long count;
    dir = path_join(root, "timeout");
    c = start_binary(dir, "./unii", "1");
    c.policy = ANSWER_NONE;
    ok(request(&c, "append", "note", big));
    hello(&c);
    j = request(&c, "turn", NULL, "WAITS_FOR_A_HUNG_CALL");
    assert(json_str(json_get(j, "error")));
    json_free(j);
    assert(c.calls == 2 && c.cancels == 2 && c.errors == 2);
    assert(strstr(c.error_text, "timed out"));
    /* A late answer to the cancelled call is refused, not fatal. */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"result\",\"call\":%lu,\"text\":\"late\"}\n",
                   ++c.serial, c.last_call) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(!strcmp(json_get(j, "ok")->s, "false") && !json_get(j, "fatal"));
    json_free(j);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "failed"), &count) && count == 2);
    ok(j);
    stop(&c);
    free(dir);
}

/* Shortest of five over-long answers is cut to the node limit (A1). */
static void oversized_answers(const char *root)
{
    Client c;
    char *dir, *line, *end;
    Json *j;
    const char *view;
    dir = path_join(root, "oversized");
    c = start(dir);
    c.policy = ANSWER_HUGE;
    ok(request(&c, "append", "note", big));
    hello(&c);
    j = request(&c, "turn", NULL, "AFTER_HUGE_SUMMARIES");
    view = json_str(json_get(j, "view"));
    assert(view && c.calls >= 5); /* on_call bounds a conversation to 5 answers */
    line = strstr(view, "0+1|");
    assert(line);
    end = strchr(line, '\n');
    assert(end && end - line == 4 + 512);
    ok(j);
    stop(&c);
    free(dir);
}

/* Turns and calls share one gate: one writer per prefix until it starts. */
static void cache_gates(const char *root, const char *text)
{
    Client c;
    char *dir;
    Json *j;
    unsigned long a, b, id, got;
    dir = path_join(root, "gates");
    c = start(dir);
    c.policy = ANSWER_MANUAL;
    ok(request(&c, "append", "note", text));
    ok(request(&c, "append", "note", text));
    hello(&c);
    while (c.nqueued < 2)
        ok(request(&c, "status", NULL, NULL));
    a = c.queued[0];
    b = c.queued[1];
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_wait\",\"key\":\"K\",\"call\":%lu}\n",
                   ++c.serial, a) > 0 && !fflush(c.out));
    ok(receive(&c));
    /* b and the turn wait on a; then a's response starts and both proceed. */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_wait\",\"key\":\"K\",\"call\":%lu}\n",
                   ++c.serial, b) > 0);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_wait\",\"key\":\"K\"}\n", ++c.serial) > 0);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_wait\",\"key\":\"K\"}\n", ++c.serial) > 0);
    id = c.serial;
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"status\"}\n", ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c); /* the duplicate turn wait is refused at once */
    assert(json_ulong(json_get(j, "id"), &got) && got == id && json_str(json_get(j, "error")));
    json_free(j);
    j = receive(&c); /* status: the two waiters are still blocked */
    assert(json_get(j, "messages"));
    json_free(j);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_started\",\"key\":\"K\",\"call\":%lu}\n",
                   ++c.serial, a) > 0 && !fflush(c.out));
    ok(receive(&c));
    ok(receive(&c));
    ok(receive(&c));
    /* A gate on an unknown call is refused. */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"cache_wait\",\"key\":\"K\",\"call\":999}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    result(&c, a, "note: first", NULL);
    result(&c, b, "note: second", NULL);
    c.policy = ANSWER_CORRECTED;
    ok(request(&c, "flush", NULL, NULL));
    stop(&c);
    free(dir);
}

/* D9: about one fsync per append. A log's directory is synced only when the
 * file is new, appended leaves never force a view save, and a batch of
 * records is one write. */
static void sync_budget(const char *root)
{
    Client c;
    char *dir, *counted, *text;
    Json *j;
    unsigned long syncs, id;
    int k;
    dir = path_join(root, "sync-budget");
    counted = path_join(root, "sync-count");
    assert(!setenv("UNII_TEST_SYNC_COUNT", counted, 1));
    c = start_binary(dir, "./tests/fsync_test", NULL);
    assert(!unsetenv("UNII_TEST_SYNC_COUNT"));
    for (k = 0; k < 20; k++)
        ok(request(&c, "append", "note", "x"));
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"append\",\"records\":[{\"kind\":\"unii\",\"text\":\"a\"},"
                   "{\"kind\":\"tool\",\"text\":\"b\"},{\"kind\":\"echo\",\"text\":\"c\"}]}\n",
                   ++c.serial) > 0 && !fflush(c.out));
    j = receive(&c);
    assert(json_ulong(json_get(j, "message_id"), &id) && id == 20);
    ok(j);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &id) && id == 23);
    ok(j);
    stop(&c);
    text = read_file(counted);
    assert(text);
    syncs = strtoul(text, NULL, 10);
    free(text);
    /* Startup: 4 directories (main, tree, the chat and its parent), the
     * first view and compaction view (6). */
    if (syncs > 10 + 21 + 1)
        fprintf(stderr, "server_test: %lu fsyncs for 21 appends\n", syncs);
    assert(syncs <= 10 + 21 + 1);
    free(dir);
    free(counted);
}

static void invalid_protocol(const char *root)
{
    Client c;
    char *dir;
    Json *j;
    Buf input;
    int status;
    dir = path_join(root, "invalid-protocol");
    c = start(dir);
    fputs("{\"id\":42,\"op\":\"append\",\"kind\":\"user\",\"text\":\"a\\u0000b\"}\n", c.out);
    assert(!fflush(c.out));
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    buf_init(&input);
    assert(read_line(c.in, &input) == 0);
    buf_free(&input);
    fclose(c.in); fclose(c.out);
    assert(waitpid(c.pid, &status, 0) == c.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    free(dir);
}

static char *daily_log(const char *dir)
{
    char date[32];
    time_t now;
    struct tm utc;
    now = time(NULL);
    assert(now != (time_t)-1 && gmtime_r(&now, &utc));
    assert(strftime(date, sizeof(date), "main/%Y-%m-%d.jsonl", &utc));
    return path_join(dir, date);
}

static void fatal_storage(const char *root)
{
    static const char first[] = "FIRST_RECORD_SURVIVES_FAILED_SYNC";
    Client c;
    char *dir, *log;
    FILE *f;
    Buf line;
    Json *j;
    unsigned long id, count;
    int status;
    dir = path_join(root, "fatal-storage");
    log = daily_log(dir);
    assert(!setenv("UNII_TEST_FSYNC_PATH", log, 1));
    c = start_binary(dir, "./tests/fsync_test", NULL);
    assert(!unsetenv("UNII_TEST_FSYNC_PATH"));
    /* Both records arrive in one buffered batch. A failed write must end it. */
    put(&c, "append", "user", first);
    put(&c, "append", "user", "SECOND_RECORD_MUST_NEVER_BE_DISPATCHED");
    assert(!fflush(c.out));
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    buf_init(&line);
    assert(read_line(c.in, &line) == 0);
    assert(!fclose(c.in) && !fclose(c.out));
    assert(waitpid(c.pid, &status, 0) == c.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    f = fopen(log, "rb");
    assert(f && read_line(f, &line) == 1);
    j = json_parse(line.s);
    assert(j && json_ulong(json_get(j, "i"), &id) && id == 0);
    assert(!strcmp(json_str(json_get(j, "text")), first));
    json_free(j);
    assert(read_line(f, &line) == 0 && !fclose(f));
    buf_free(&line);
    /* The completed crash tail remains recoverable with its original id. */
    c = start(dir);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 1);
    ok(j);
    stop(&c);
    free(dir); free(log);
}

static void worker_read_fault(const char *root, const char *text)
{
    Client c;
    char *dir, *log;
    Buf line;
    Json *j;
    FILE *f;
    unsigned long count;
    int status;
    dir = path_join(root, "worker-read-fault");
    log = daily_log(dir);
    c = start(dir);
    ok(request(&c, "append", "note", text));
    /* The log becomes unreadable once written: an engine read must fail
     * fatally, not build from nothing (skipped when run as root). */
    assert(!chmod(log, 0));
    if (access(log, R_OK) == 0) {
        assert(!chmod(log, 0600));
        stop(&c);
        free(dir); free(log);
        return;
    }
    put(&c, "hello", NULL, NULL);
    put(&c, "flush", NULL, NULL);
    assert(!fflush(c.out));
    ok(receive(&c));
    /* Sizes alone say the note needs a model; reading it for the task fails. */
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    assert(!strcmp(json_get(j, "fatal")->s, "true"));
    json_free(j);
    buf_init(&line);
    assert(read_line(c.in, &line) == 0);
    assert(!fclose(c.in) && !fclose(c.out));
    assert(waitpid(c.pid, &status, 0) == c.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    assert(!chmod(log, 0600));
    f = fopen(log, "rb");
    assert(f && read_line(f, &line) == 1);
    j = json_parse(line.s);
    assert(j && !strcmp(json_str(json_get(j, "text")), text));
    json_free(j);
    assert(read_line(f, &line) == 0 && !fclose(f));
    buf_free(&line);
    c = start(dir);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 1);
    ok(j);
    stop(&c);
    free(dir); free(log);
}

/* The view covers [0, cut) with aligned summaries; any [cut, total) must
 * come whole in recent, within TAIL_BYTES (spec change C2). */
static void check_coverage(const Json *turn, unsigned long total)
{
    char *p, *end, head[64];
    const char *view, *recent;
    unsigned long id, n, covered;
    view = json_str(json_get(turn, "view"));
    recent = json_str(json_get(turn, "recent"));
    assert(view && strlen(view) <= 128000);
    assert(!strstr(view, "not summarized yet"));
    covered = 0;
    for (p = (char *)view + 7; *p != '<'; p = end + 1) {
        id = strtoul(p, &end, 10);
        assert(end != p && *end == '+');
        n = strtoul(end + 1, &end, 10);
        assert(*end == '|' && n && !(n & (n - 1)));
        assert(id == covered && !(id % n) && n <= total - covered);
        covered += n;
        end = strchr(end, '\n');
        assert(end);
    }
    assert(!strcmp(p, "</chat>\n"));
    if (!recent) {
        assert(covered == total);
        return;
    }
    assert(covered < total && strlen(recent) <= 32000);
    sprintf(head, "<recent>\n[message %lu kind=", covered);
    assert(!strncmp(recent, head, strlen(head)));
    sprintf(head, "\n[message %lu kind=", total - 1);
    assert(strstr(recent, head));
}

static void backlog(const char *root, const char *source, int fail_merges)
{
    Client c;
    char *dir, *marker, short_source[501];
    const char *view;
    Json *j;
    FILE *f;
    unsigned long count;
    int k;
    dir = path_join(root, fail_merges ? "failed-backlog" : "backlog");
    marker = path_join(root, "fail-merges");
    c = start(dir);
    c.policy = ANSWER_500;
    if (fail_merges) {
        /* Verbatim leaves can all build even while every parent fails. */
        memcpy(short_source, source, 500);
        short_source[500] = 0;
        source = short_source;
        f = fopen(marker, "w");
        assert(f && !fclose(f));
        c.fail_merges = marker;
    }
    for (k = 0; k < 500; k++)
        ok(request(&c, "append", "note", source));
    put(&c, "hello", NULL, NULL);
    put(&c, "turn", NULL, "NEW_INPUT_AFTER_LARGE_BACKLOG");
    assert(!fflush(c.out));
    ok(receive(&c));
    j = receive(&c);
    if (fail_merges) {
        assert(json_str(json_get(j, "error")) && !json_get(j, "view"));
        json_free(j);
        j = request(&c, "status", NULL, NULL);
        assert(json_ulong(json_get(j, "messages"), &count) && count == 501);
        assert(json_ulong(json_get(j, "summarized"), &count) && count == 501);
        assert(json_ulong(json_get(j, "failed"), &count) && count > 0);
        assert(json_ulong(json_get(j, "blocked"), &count) && count > 0);
        ok(j);
        /* Once the fault clears, a new message retries the failed parents
         * and the saved input is covered by a bounded preceding view. */
        assert(!unlink(marker));
        j = request(&c, "turn", NULL, "RETRY_INPUT_AFTER_PARENT_FAILURE");
        view = json_str(json_get(j, "view"));
        check_coverage(j, 501);
        assert(!strstr(view, "RETRY_INPUT"));
    } else {
        view = json_str(json_get(j, "view"));
        check_coverage(j, 500);
        assert(!json_get(j, "recent") || !strstr(json_str(json_get(j, "recent")), "NEW_INPUT"));
        assert(!strstr(view, "NEW_INPUT"));
    }
    ok(j);
    stop(&c);
    free(dir); free(marker);
}

int main(int argc, char **argv)
{
    Client c;
    Json *j;
    char text[1001], *before, *after, *path;
    const char *view;
    unsigned long count;
    int k;
    assert(argc == 2);
    memset(text, 'L', 1000); text[1000] = 0;
    big = xmalloc(40001);
    memset(big, 'B', 40000); big[40000] = 0;
    c = start(argv[1]);
    hello(&c);
    j = request(&c, "turn", NULL, "first user message");
    view = json_str(json_get(j, "view"));
    assert(view && !strcmp(view, "<chat>\n</chat>\n"));
    ok(j);
    for (k = 0; k < 16; k++) ok(request(&c, "append", "unii", text));
    ok(request(&c, "append", "tool", "read {\"path\":\"file.c\"}"));
    ok(request(&c, "append", "echo", "tool output"));
    ok(request(&c, "steer", NULL, "a correction between tool calls"));
    ok(request(&c, "flush", NULL, NULL));
    assert(c.calls > 16); /* corrections and merges were real calls */
    assert(c.appends == 20 && c.builts == 20 + 10 + 5 + 2 + 1);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 20);
    assert(json_ulong(json_get(j, "summarized"), &count) && count == 20);
    assert(!strcmp(json_str(json_get(j, "summary_model")), "test-model"));
    ok(j);
    read_beside(argv[1]);
    j = request(&c, "turn", NULL, "new message must follow the saved view");
    view = json_str(json_get(j, "view"));
    assert(view && strstr(view, "0+1|user: first user message") && !json_get(j, "recent"));
    assert(!strstr(view, "new message must follow"));
    assert(!strstr(view, "not summarized yet"));
    ok(j);
    /* zoom answers carry a short reference for the log (C4). */
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"zoom\",\"args\":{\"id\":0,\"n\":1}}\n", ++c.serial) > 0);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"zoom\",\"args\":{\"id\":0,\"n\":2}}\n", ++c.serial) > 0);
    assert(fprintf(c.out, "{\"id\":%lu,\"op\":\"zoom\",\"args\":{\"id\":3,\"n\":2}}\n", ++c.serial) > 0);
    assert(!fflush(c.out));
    j = receive(&c);
    assert(strstr(json_str(json_get(j, "text")), "first user message"));
    assert(!strcmp(json_str(json_get(j, "log")), "zoom {\"id\":0,\"n\":1,\"page\":0}: "
                   "message 0 page 0/1, 18 bytes; not copied; zoom again to read"));
    ok(j);
    j = receive(&c);
    assert(!strncmp(json_str(json_get(j, "log")), "zoom {\"id\":0,\"n\":2,\"page\":0}: "
                    "lines 0+1|user: first user message; 1+1|", 59));
    ok(j);
    j = receive(&c);
    assert(json_str(json_get(j, "error")) && !json_get(j, "log") && !json_get(j, "fatal"));
    json_free(j);
    /* Arguments come only in "args": a top-level id is the correlation id. */
    assert(fputs("{\"id\":0,\"op\":\"zoom\",\"n\":1}\n", c.out) >= 0 && !fflush(c.out));
    j = receive(&c);
    assert(!strcmp(json_str(json_get(j, "text")), "zoom: id, n and page must be nonnegative integers"));
    ok(j);
    ok(request(&c, "flush", NULL, NULL));
    path = path_join(argv[1], "view.json");
    before = read_file(path);
    assert(before);
    stop(&c);
    failed_input(argv[1], 0);
    failed_input(argv[1], 1);
    timed_out_call(argv[1]);
    history(argv[1], text);
    recent_tail(argv[1], text);
    flush_then_turn(argv[1]);
    shared_socket(argv[1]);
    parent_sync(argv[1]);
    explicit_retry(argv[1], text);
    oversized_answers(argv[1]);
    cache_gates(argv[1], text);
    sync_budget(argv[1]);
    invalid_protocol(argv[1]);
    fatal_storage(argv[1]);
    worker_read_fault(argv[1], text);
    backlog(argv[1], text, 0);
    backlog(argv[1], text, 1);
    c = start(argv[1]);
    hello(&c);
    after = read_file(path);
    assert(after && !strcmp(before, after));
    free(before); free(after); free(path); free(big);
    stop(&c);
    puts("server: host calls, corrections, ceiling, timeouts, gates, steering, restart OK");
    return 0;
}
