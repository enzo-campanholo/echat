#include "base.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct Client { FILE *in, *out; pid_t pid; unsigned long serial; } Client;

static Client start_binary(const char *dir, const char *binary)
{
    int in[2], out[2];
    Client c;
    assert(!pipe(in) && !pipe(out));
    c.pid = fork();
    assert(c.pid >= 0);
    if (!c.pid) {
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl(binary, "unii", "serve", dir, "--pi", "./tests/fake_pi", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    c.in = fdopen(out[0], "r");
    c.out = fdopen(in[1], "w");
    c.serial = 0;
    assert(c.in && c.out);
    return c;
}

static Client start(const char *dir)
{
    return start_binary(dir, "./unii");
}

static void send(Client *c, const char *op, const char *kind, const char *text)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, ++c->serial);
    buf_puts(&b, ",\"op\":"); buf_json(&b, op);
    if (kind) { buf_puts(&b, ",\"kind\":"); buf_json(&b, kind); }
    if (text) {
        buf_puts(&b, !strcmp(op, "configure") ? ",\"system\":" : ",\"text\":");
        buf_json(&b, text);
    }
    buf_puts(&b, "}\n");
    fputs(b.s, c->out);
    buf_free(&b);
}

static Json *receive(Client *c)
{
    Buf input;
    Json *response;
    buf_init(&input);
    assert(read_line(c->in, &input) == 1);
    response = json_parse(input.s);
    assert(response);
    buf_free(&input);
    return response;
}

static Json *request(Client *c, const char *op, const char *kind, const char *text)
{
    send(c, op, kind, text);
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

static void stop(Client *c)
{
    int status;
    ok(request(c, "shutdown", NULL, NULL));
    fclose(c->out); fclose(c->in);
    assert(waitpid(c->pid, &status, 0) == c->pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static void failed_input(const char *root, const char *text)
{
    Client c;
    char *dir, *marker;
    Json *j;
    unsigned long count;
    const char *view;
    dir = path_join(root, "failed-input");
    marker = path_join(root, "fail-once");
    assert(!setenv("UNII_TEST_FAIL_ONCE", marker, 1));
    c = start(dir);
    assert(!unsetenv("UNII_TEST_FAIL_ONCE"));
    ok(request(&c, "append", "note", text));
    /* Dispatch both before the workers run, making the failure deterministic. */
    send(&c, "configure", NULL, "fixed system");
    send(&c, "turn", NULL, "INPUT_SAVED_DESPITE_SUMMARY_FAILURE");
    assert(!fflush(c.out));
    ok(receive(&c));
    j = receive(&c);
    assert(json_str(json_get(j, "error")));
    json_free(j);
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 2);
    ok(j);
    j = request(&c, "turn", NULL, "retry after failure");
    view = json_str(json_get(j, "view"));
    assert(view && strstr(view, "INPUT_SAVED_DESPITE_SUMMARY_FAILURE"));
    assert(!strstr(view, "retry after failure") && !strstr(view, "not summarized yet"));
    ok(j);
    stop(&c);
    free(dir); free(marker);
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

static void fatal_storage(const char *root)
{
    static const char first[] = "FIRST_RECORD_SURVIVES_FAILED_SYNC";
    Client c;
    char date[32], *dir, *log;
    time_t now;
    struct tm utc;
    FILE *f;
    Buf line;
    Json *j;
    unsigned long id, count;
    int status;
    dir = path_join(root, "fatal-storage");
    now = time(NULL);
    assert(now != (time_t)-1 && gmtime_r(&now, &utc));
    assert(strftime(date, sizeof(date), "main/%Y-%m-%d.jsonl", &utc));
    log = path_join(dir, date);
    assert(!setenv("UNII_TEST_FSYNC_PATH", log, 1));
    c = start_binary(dir, "./tests/fsync_test");
    assert(!unsetenv("UNII_TEST_FSYNC_PATH"));
    /* Both records arrive in one buffered batch. A failed write must end it. */
    send(&c, "append", "user", first);
    send(&c, "append", "user", "SECOND_RECORD_MUST_NEVER_BE_DISPATCHED");
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

static void backlog(const char *root, const char *source)
{
    Client c;
    char *dir, *p, *end;
    const char *view;
    Json *j;
    unsigned long id, n, covered;
    int k;
    dir = path_join(root, "backlog");
    assert(!setenv("UNII_TEST_SUMMARY_BYTES", "500", 1));
    c = start(dir);
    assert(!unsetenv("UNII_TEST_SUMMARY_BYTES"));
    for (k = 0; k < 500; k++)
        ok(request(&c, "append", "note", source));
    send(&c, "configure", NULL, "fixed system");
    send(&c, "turn", NULL, "NEW_INPUT_AFTER_LARGE_BACKLOG");
    assert(!fflush(c.out));
    ok(receive(&c));
    j = receive(&c);
    view = json_str(json_get(j, "view"));
    assert(view && strlen(view) <= 128000);
    assert(!strstr(view, "not summarized yet") && !strstr(view, "NEW_INPUT"));
    covered = 0;
    for (p = (char *)view + 7; *p != '<'; p = end + 1) {
        id = strtoul(p, &end, 10);
        assert(end != p && *end == '+');
        n = strtoul(end + 1, &end, 10);
        assert(*end == '|' && n && !(n & (n - 1)));
        assert(id == covered && !(id % n) && n <= 500 - covered);
        covered += n;
        end = strchr(end, '\n');
        assert(end);
    }
    assert(covered == 500 && !strcmp(p, "</chat>\n"));
    ok(j);
    stop(&c);
    free(dir);
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
    c = start(argv[1]);
    ok(request(&c, "configure", NULL, "fixed system"));
    j = request(&c, "turn", NULL, "first user message");
    view = json_str(json_get(j, "view"));
    assert(view && !strcmp(view, "<chat>\n</chat>\n"));
    ok(j);
    for (k = 0; k < 16; k++) ok(request(&c, "append", "unii", text));
    ok(request(&c, "append", "tool", "read {\"path\":\"file.c\"}"));
    ok(request(&c, "append", "echo", "tool output"));
    ok(request(&c, "steer", NULL, "a correction between tool calls"));
    ok(request(&c, "flush", NULL, NULL));
    j = request(&c, "status", NULL, NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 20);
    assert(json_ulong(json_get(j, "summarized"), &count) && count == 20);
    ok(j);
    j = request(&c, "turn", NULL, "new message must follow the saved view");
    view = json_str(json_get(j, "view"));
    assert(view && strstr(view, "0+1|user: first user message"));
    assert(!strstr(view, "new message must follow"));
    assert(!strstr(view, "never log this thought"));
    assert(!strstr(view, "not summarized yet"));
    before = xstrdup(view);
    ok(j);
    ok(request(&c, "flush", NULL, NULL));
    path = path_join(argv[1], "view.json");
    free(before);
    before = read_file(path);
    assert(before);
    stop(&c);
    failed_input(argv[1], text);
    invalid_protocol(argv[1]);
    fatal_storage(argv[1]);
    backlog(argv[1], text);
    c = start(argv[1]);
    ok(request(&c, "configure", NULL, "fixed system"));
    after = read_file(path);
    assert(after && !strcmp(before, after));
    free(before); free(after); free(path);
    stop(&c);
    puts("server: fresh views, concurrent compactions, byte retries, steering, restart OK");
    return 0;
}
