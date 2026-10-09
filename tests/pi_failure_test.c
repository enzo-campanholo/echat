#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Client { FILE *in, *out; pid_t pid; } Client;
static pid_t running;
static int summary_notices;
static char *summary_notice;
static const char saved_input[] = "WORDS_SURVIVE_A_FAILED_PREDECESSOR";

static void timeout_handler(int sig)
{
    static const char text[] = "Pi failure test exceeded 45 seconds\n";
    (void)sig;
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

/* A real backend with injected summary errors or a failed append. */
static int proxy(int argc, char **argv)
{
    int input[2], output[2], status, n, r, appends, fail_append, summary_errors;
    pid_t pid;
    struct pollfd fds[2];
    char bytes[4096], *end;
    const char *op;
    Json *j;
    Buf pending;
    static const char summary_error[] =
        "{\"event\":\"error\",\"provider\":\"openai-codex\",\"model\":\"gpt-6-luna\","
        "\"text\":\"OAuth refresh failed\\nTEST_STACK\\n\"}\n";
    assert(argc > 1 && getenv("UNII_TEST_REAL_BIN"));
    appends = 0;
    summary_errors = getenv("UNII_TEST_SUMMARY_ERRORS") != NULL;
    fail_append = getenv("UNII_TEST_FAIL_APPEND") ? atoi(getenv("UNII_TEST_FAIL_APPEND")) : 1;
    assert(fail_append > 0);
    if (strcmp(argv[1], "serve")) {
        execv(getenv("UNII_TEST_REAL_BIN"), argv);
        return 127;
    }
    assert(!pipe(input) && !pipe(output));
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execv(getenv("UNII_TEST_REAL_BIN"), argv);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    fds[0].fd = 0; fds[0].events = POLLIN;
    fds[1].fd = output[0]; fds[1].events = POLLIN;
    buf_init(&pending);
    for (;;) {
        r = poll(fds, 2, -1);
        if (r < 0 && errno == EINTR) continue;
        assert(r > 0);
        if (fds[1].revents) {
            n = (int)read(output[0], bytes, sizeof(bytes));
            if (n <= 0) break;
            assert(write_all(1, bytes, (size_t)n) == 0);
        }
        if (fds[0].revents) {
            n = (int)read(0, bytes, sizeof(bytes));
            if (n <= 0) break;
            buf_add(&pending, bytes, (size_t)n);
            while ((end = strchr(pending.s, '\n')) != NULL) {
                *end = 0;
                j = json_parse(pending.s);
                assert(j);
                op = json_str(json_get(j, "op"));
                if (summary_errors && op && !strcmp(op, "configure")) {
                    assert(write_all(1, summary_error, sizeof(summary_error) - 1) == 0);
                    assert(write_all(1, summary_error, sizeof(summary_error) - 1) == 0);
                    summary_errors = 0;
                }
                if (op && !strcmp(op, "append") && ++appends == fail_append) {
                    json_free(j);
                    kill(pid, SIGTERM);
                    assert(waitpid(pid, &status, 0) == pid);
                    buf_free(&pending);
                    close(input[1]); close(output[0]);
                    return 42;
                }
                json_free(j);
                *end = '\n';
                n = (int)(end - pending.s + 1);
                assert(write_all(input[1], pending.s, (size_t)n) == 0);
                memmove(pending.s, pending.s + n, pending.len - (size_t)n + 1);
                pending.len -= (size_t)n;
            }
        }
    }
    close(input[1]); close(output[0]);
    assert(waitpid(pid, &status, 0) == pid);
    buf_free(&pending);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static Client start(const char *project)
{
    Client c;
    int input[2], output[2];
    char *extension, *provider;
    summary_notices = 0;
    free(summary_notice); summary_notice = NULL;
    extension = path_join(project, "pi/unii.ts");
    provider = path_join(project, "tests/pi_failure_model.ts");
    assert(!pipe(input) && !pipe(output));
    c.pid = fork();
    assert(c.pid >= 0);
    if (!c.pid) {
        assert(setpgid(0, 0) == 0);
        assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", extension, "--extension", provider,
               "--provider", "unii-failure", "--model", "deterministic",
               "--tools", "probe,zoom,date", (char *)NULL);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    c.in = fdopen(output[0], "r"); c.out = fdopen(input[1], "w");
    assert(c.in && c.out);
    free(extension); free(provider);
    running = c.pid;
    alarm(45);
    return c;
}

static void send_prompt(Client *c, unsigned long id, const char *text)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"type\":\"prompt\",\"id\":"); buf_ulong(&b, id);
    buf_puts(&b, ",\"message\":"); buf_json(&b, text); buf_puts(&b, "}\n");
    assert(fwrite(b.s, 1, b.len, c->out) == b.len && !fflush(c->out));
    buf_free(&b);
}

static void accepted(Client *c, const char *disposition)
{
    Buf line;
    Json *j;
    const char *type, *actual, *message;
    int done;
    done = 0;
    buf_init(&line);
    while (read_line(c->in, &line) == 1) {
        j = json_parse(line.s);
        assert(j);
        type = json_str(json_get(j, "type"));
        message = json_str(json_get(j, "message"));
        if (type && !strcmp(type, "extension_ui_request") &&
            message && strstr(message, "UniiChat summary failed")) {
            summary_notices++;
            free(summary_notice); summary_notice = xstrdup(message);
        }
        if (type && !strcmp(type, "response")) {
            actual = json_str(json_get(json_get(j, "data"), "disposition"));
            if (!actual || strcmp(actual, disposition)) {
                fprintf(stderr, "unexpected Pi prompt response: %s\n", line.s);
                assert(0);
            }
            done = 1;
        }
        json_free(j);
        if (done) break;
    }
    assert(done);
    buf_free(&line);
}

static void settled(Client *c)
{
    Buf line;
    Json *j;
    const char *type;
    int done;
    done = 0;
    buf_init(&line);
    while (read_line(c->in, &line) == 1) {
        j = json_parse(line.s); assert(j);
        type = json_str(json_get(j, "type"));
        done = type && !strcmp(type, "agent_settled");
        json_free(j);
        if (done) break;
    }
    assert(done);
    buf_free(&line);
}

static void stop(Client *c)
{
    int status;
    assert(!fclose(c->out));
    while (fgetc(c->in) != EOF) ;
    assert(!fclose(c->in));
    assert(waitpid(c->pid, &status, 0) == c->pid);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static size_t lines(const char *path)
{
    FILE *f;
    Buf b;
    size_t count;
    f = fopen(path, "rb");
    if (!f) { assert(errno == ENOENT); return 0; }
    buf_init(&b);
    count = 0;
    while (read_line(f, &b) == 1) count++;
    buf_free(&b); assert(!fclose(f));
    return count;
}

static void check_calls(const char *dir, size_t provider, size_t tools)
{
    char *path;
    path = path_join(dir, "provider.jsonl");
    assert(lines(path) == provider); free(path);
    path = path_join(dir, "probe.txt");
    assert(lines(path) == tools); free(path);
}

static char *setup(const char *project, const char *dir, const char *name)
{
    char *path;
    path = path_join(dir, name);
    assert(!mkdirs(path));
    set_path("UNII_BIN", project, "unii");
    set_path("UNII_PI", project, "tests/fake_pi");
    set_path("UNII_DIR", path, "chat");
    set_path("UNII_TEST_PROVIDER_LOG", path, "provider.jsonl");
    set_path("UNII_TEST_PROBE", path, "probe.txt");
    set_path("PI_CODING_AGENT_DIR", path, "pi-config");
    assert(!unsetenv("UNII_TEST_FAIL_ONCE"));
    assert(!unsetenv("UNII_TEST_ZOOM"));
    assert(!unsetenv("UNII_TEST_MISSING_LOG"));
    assert(!unsetenv("UNII_TEST_SUMMARY_ERRORS") && !unsetenv("UNII_TEST_FAIL_APPEND"));
    return path;
}

static char *seed_note(const char *dir)
{
    static const char record[] =
        "{\"i\":0,\"kind\":\"note\",\"text\":\"PRIOR_NOTE\",\"size\":10,"
        "\"date\":\"2000-01-01T00:00:00Z\"}\n";
    Store s;
    char *chat, *log;
    FILE *f;
    chat = path_join(dir, "chat");
    assert(store_open(&s, chat));
    store_close(&s);
    log = path_join(chat, "main/2000-01-01.jsonl");
    free(chat);
    f = fopen(log, "wb");
    assert(f && fwrite(record, 1, sizeof(record) - 1, f) == sizeof(record) - 1);
    assert(!fclose(f));
    return log;
}

static void saved_words(const char *dir)
{
    Store s;
    char *path, *text;
    unsigned long i;
    int count;
    path = path_join(dir, "chat");
    assert(store_open(&s, path)); free(path);
    count = 0;
    for (i = 0; i < s.count; i++) {
        text = store_message(&s, i); assert(text);
        if (!strcmp(text, saved_input)) count++;
        free(text);
    }
    assert(count == 1);
    store_close(&s);
}

int main(int argc, char **argv)
{
    char project[4096], note[1001], *dir, *chat, *log, *hidden;
    Buf path;
    Client c;
    Store s;
    assert(argc > 1);
    if (!strcmp(argv[1], "prompt") || !strcmp(argv[1], "serve"))
        return proxy(argc, argv);
    assert(argc == 2 && getcwd(project, sizeof(project)));
    assert(!mkdirs(argv[1]));
    assert(!setenv("PI_OFFLINE", "1", 1));
    assert(!unsetenv("UNII_INSTRUCTIONS"));
    assert(!unsetenv("UNII_TEST_COMPACTION_LOG"));
    assert(!unsetenv("UNII_TEST_COMPACTION_IMAGES"));
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);

    dir = setup(project, argv[1], "missing");
    assert(!setenv("UNII_BIN", "/definitely/missing/unii-test", 1));
    c = start(project);
    send_prompt(&c, 1, "MUST_STOP_WITHOUT_MEMORY"); accepted(&c, "handled");
    stop(&c); check_calls(dir, 0, 0); free(dir);

    dir = setup(project, argv[1], "summary-errors");
    set_path("UNII_TEST_REAL_BIN", project, "unii");
    set_path("UNII_BIN", project, "tests/pi_failure_test");
    assert(!setenv("UNII_TEST_FAIL_APPEND", "100", 1));
    assert(!setenv("UNII_TEST_SUMMARY_ERRORS", "1", 1));
    c = start(project);
    send_prompt(&c, 1, "SUMMARY_ERRORS_MUST_NOT_STOP_THE_REPLY"); accepted(&c, "started");
    assert(summary_notices == 1 && summary_notice);
    assert(strstr(summary_notice, "openai-codex/gpt-6-luna"));
    assert(strstr(summary_notice, "Use /login for openai-codex"));
    assert(!strstr(summary_notice, "TEST_STACK"));
    settled(&c); stop(&c); check_calls(dir, 2, 1); free(dir);

    dir = setup(project, argv[1], "predecessor");
    set_path("UNII_TEST_FAIL_ONCE", dir, "failed-once");
    chat = path_join(dir, "chat");
    assert(store_open(&s, chat)); free(chat);
    memset(note, 'L', 1000); note[1000] = 0;
    assert(store_append(&s, "note", note, NULL, NULL));
    store_close(&s);
    c = start(project);
    send_prompt(&c, 1, saved_input); accepted(&c, "handled");
    check_calls(dir, 0, 0);
    send_prompt(&c, 2, "RETRY_WITH_COMPLETE_MEMORY"); accepted(&c, "started");
    settled(&c); stop(&c);
    check_calls(dir, 2, 1); saved_words(dir); free(dir);

    dir = setup(project, argv[1], "nul");
    c = start(project);
    assert(fputs("{\"type\":\"prompt\",\"id\":1,\"message\":\"a\\u0000b\"}\n", c.out) >= 0);
    assert(!fflush(c.out)); accepted(&c, "handled");
    stop(&c); check_calls(dir, 0, 0); free(dir);

    dir = setup(project, argv[1], "append");
    set_path("UNII_TEST_REAL_BIN", project, "unii");
    set_path("UNII_BIN", project, "tests/pi_failure_test");
    c = start(project);
    send_prompt(&c, 1, saved_input); accepted(&c, "started");
    settled(&c); stop(&c);
    check_calls(dir, 1, 0); saved_words(dir); free(dir);

    dir = setup(project, argv[1], "missing-log");
    log = seed_note(dir);
    assert(!setenv("UNII_TEST_ZOOM", "missing", 1));
    assert(!setenv("UNII_TEST_MISSING_LOG", log, 1));
    c = start(project);
    send_prompt(&c, 1, saved_input); accepted(&c, "started");
    settled(&c); stop(&c);
    check_calls(dir, 1, 0);
    buf_init(&path); buf_puts(&path, log); buf_puts(&path, ".hidden");
    hidden = path.s;
    assert(!rename(hidden, log));
    buf_free(&path);
    saved_words(dir); free(log); free(dir);

    dir = setup(project, argv[1], "invalid-zoom");
    assert(!setenv("UNII_TEST_ZOOM", "range", 1));
    c = start(project);
    send_prompt(&c, 1, saved_input); accepted(&c, "started");
    settled(&c); stop(&c);
    check_calls(dir, 3, 1); saved_words(dir); free(dir);
    free(summary_notice);
    puts("live Pi failures: summary notices, durable input, blocked tools, fatal reads, recoverable zoom errors OK");
    return 0;
}
