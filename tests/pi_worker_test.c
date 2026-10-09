#include "base.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t running;

static void timeout_handler(int signal_number)
{
    static const char text[] = "real Pi workers exceeded 60 seconds\n";
    (void)signal_number;
    if (running > 0) kill(-running, SIGKILL);
    write(2, text, sizeof(text) - 1);
    _exit(124);
}

static void set_path(const char *name, const char *dir, const char *file)
{
    char *path;
    path = path_join(dir, file);
    assert(setenv(name, path, 1) == 0);
    free(path);
}

static void start(FILE **input, FILE **output, const char *dir)
{
    int in[2], out[2];
    char *chat;
    assert(pipe(in) == 0 && pipe(out) == 0);
    chat = path_join(dir, "chat");
    running = fork();
    assert(running >= 0);
    if (!running) {
        assert(setpgid(0, 0) == 0);
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("./unii", "unii", "serve", chat, "--pi", "./tests/pi_worker_pi.sh",
              "--provider", "unii-worker-test", "--model", "deterministic", (char *)NULL);
        _exit(127);
    }
    free(chat);
    close(in[0]); close(out[1]);
    *input = fdopen(out[0], "r");
    *output = fdopen(in[1], "w");
    assert(*input && *output);
}

static Json *request(FILE *input, FILE *output, unsigned long *serial,
                     const char *op, const char *text)
{
    Buf b, line;
    Json *j;
    const Json *flag;
    buf_init(&b); buf_init(&line);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, ++*serial);
    buf_puts(&b, ",\"op\":"); buf_json(&b, op);
    if (text) {
        buf_puts(&b, !strcmp(op, "configure") ? ",\"system\":" : ",\"kind\":\"note\",\"text\":");
        buf_json(&b, text);
    }
    buf_puts(&b, "}\n");
    assert(fputs(b.s, output) >= 0 && fflush(output) == 0);
    assert(read_line(input, &line) == 1);
    j = json_parse(line.s);
    assert(j);
    flag = json_get(j, "ok");
    if (!flag || flag->type != J_BOOL || strcmp(flag->s, "true")) {
        fprintf(stderr, "real worker backend failed: %s\n", line.s);
        assert(0);
    }
    buf_free(&b); buf_free(&line);
    return j;
}

static void contexts(const char *dir)
{
    char *path, *base, *current;
    const char *text;
    FILE *f;
    Buf line, encoded;
    Json *j;
    const Json *context, *message, *payload, *block, *system, *control;
    unsigned long pid, pids[8];
    size_t counts[8], n, k, total, gated, marked;
    path = path_join(dir, "worker.jsonl");
    f = fopen(path, "rb");
    assert(f);
    free(path);
    n = total = gated = 0;
    base = NULL;
    memset(counts, 0, sizeof(counts));
    buf_init(&line);
    while (read_line(f, &line) == 1) {
        j = json_parse(line.s);
        assert(j && json_ulong(json_get(j, "pid"), &pid));
        for (k = 0; k < n && pids[k] != pid; k++) ;
        if (k == n) { assert(n < 8); pids[n++] = pid; }
        counts[k]++;
        context = json_get(j, "context");
        assert(context && context->type == J_ARR);
        message = context->child;
        assert(message && !strcmp(json_str(json_get(message, "role")), "system"));
        assert(!strcmp(json_str(json_get(message, "content")), "fixed worker system"));
        message = message->next;
        assert(message && !message->next && !strcmp(json_str(json_get(message, "role")), "user"));
        payload = json_get(j, "payload");
        system = json_get(payload, "system");
        assert(system && system->type == J_ARR && system->child);
        for (block = system->child; block->next; block = block->next) ;
        control = json_get(block, "cache_control");
        assert(control && !strcmp(json_str(json_get(control, "type")), "ephemeral"));
        assert(!json_get(control, "ttl"));
        buf_init(&encoded);
        json_dump(&encoded, system);
        current = encoded.s;
        if (!base) base = xstrdup(current);
        assert(!strcmp(base, current));
        free(current);
        message = json_get(payload, "messages")->child;
        marked = 0;
        for (block = json_get(message, "content")->child; block; block = block->next) {
            text = json_str(json_get(block, "text"));
            assert(text && !strstr(text, "not summarized yet"));
            if (json_get(block, "cache_control")) marked++;
        }
        assert(marked >= 1);
        if (marked == 2) gated++;
        total++;
        json_free(j);
    }
    assert(total == 31 && gated > 0);
    for (k = 0; k < n; k++) if (counts[k] > 1) break;
    assert(k < n);
    free(base);
    buf_free(&line);
    assert(fclose(f) == 0);
}

int main(int argc, char **argv)
{
    char project[4096], text[1001];
    FILE *input, *output;
    Json *j;
    unsigned long serial, count;
    int k, status;
    assert(argc == 2 && getcwd(project, sizeof(project)));
    set_path("UNII_TEST_WORKER_PROVIDER", project, "tests/pi_worker_model.ts");
    set_path("UNII_TEST_WORKER_LOG", argv[1], "worker.jsonl");
    set_path("PI_CODING_AGENT_DIR", argv[1], "pi-config");
    assert(setenv("PI_OFFLINE", "1", 1) == 0);
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);
    start(&input, &output, argv[1]);
    alarm(60);
    serial = 0;
    json_free(request(input, output, &serial, "configure", "fixed worker system"));
    memset(text, 'L', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    for (k = 0; k < 16; k++)
        json_free(request(input, output, &serial, "append", text));
    json_free(request(input, output, &serial, "flush", NULL));
    j = request(input, output, &serial, "status", NULL);
    assert(json_ulong(json_get(j, "messages"), &count) && count == 16);
    assert(json_ulong(json_get(j, "summarized"), &count) && count == 16);
    assert(json_ulong(json_get(j, "failed"), &count) && count == 0);
    json_free(j);
    json_free(request(input, output, &serial, "shutdown", NULL));
    assert(fclose(output) == 0 && fclose(input) == 0);
    assert(waitpid(running, &status, 0) == running);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    contexts(argv[1]);
    puts("real Pi workers: 31 compactions, reused sessions, fresh context, cache barriers OK");
    return 0;
}
