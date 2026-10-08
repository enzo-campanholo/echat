#include "base.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct Client { FILE *in, *out; pid_t pid; unsigned long serial; } Client;

static Client start(const char *dir)
{
    int in[2], out[2];
    Client c;
    assert(!pipe(in) && !pipe(out));
    c.pid = fork();
    assert(c.pid >= 0);
    if (!c.pid) {
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("./unii", "unii", "serve", dir, "--pi", "./tests/fake_pi", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    c.in = fdopen(out[0], "r");
    c.out = fdopen(in[1], "w");
    c.serial = 0;
    assert(c.in && c.out);
    return c;
}

static Json *request(Client *c, const char *op, const char *kind, const char *text)
{
    Buf b, input;
    Json *response;
    buf_init(&b); buf_init(&input);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, ++c->serial);
    buf_puts(&b, ",\"op\":"); buf_json(&b, op);
    if (kind) { buf_puts(&b, ",\"kind\":"); buf_json(&b, kind); }
    if (text) {
        buf_puts(&b, !strcmp(op, "configure") ? ",\"system\":" : ",\"text\":");
        buf_json(&b, text);
    }
    buf_puts(&b, "}\n");
    fputs(b.s, c->out); assert(!fflush(c->out));
    assert(read_line(c->in, &input) == 1);
    response = json_parse(input.s);
    assert(response);
    buf_free(&b); buf_free(&input);
    return response;
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
    c = start(argv[1]);
    ok(request(&c, "configure", NULL, "fixed system"));
    after = read_file(path);
    assert(after && !strcmp(before, after));
    free(before); free(after); free(path);
    stop(&c);
    puts("server: fresh views, concurrent compactions, byte retries, steering, restart OK");
    return 0;
}
