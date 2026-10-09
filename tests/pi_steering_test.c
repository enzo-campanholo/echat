#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char first[] = "FIRST_INPUT_WHILE_MEMORY_WAITS";
static const char waiting[] = "SECOND_INPUT_DURING_PREPARATION";
static const char active[] = "THIRD_INPUT_DURING_REPLY";
static const char following[] = "FOURTH_INPUT_IN_FRESH_TURN";
static pid_t running;

static void timeout_handler(int sig)
{
    static const char text[] = "Pi steering test exceeded 45 seconds\n";
    (void)sig;
    if (running > 0) kill(-running, SIGKILL);
    write(2, text, sizeof(text) - 1);
    _exit(124);
}

static void gate(const char *path, int read_side)
{
    FILE *f;
    f = fopen(path, read_side ? "rb" : "wb");
    assert(f);
    if (read_side) assert(fgetc(f) == EOF);
    assert(!fclose(f));
}

/* A delayed compactor. FIFOs make the waiting-input race deterministic. */
static int compactor(void)
{
    Buf input, response;
    Json *j;
    const char *type;
    FILE *f;
    int first_call;
    first_call = 1;
    buf_init(&input); buf_init(&response);
    while (read_line(stdin, &input) == 1) {
        j = json_parse(input.s); assert(j);
        type = json_str(json_get(j, "type"));
        buf_reset(&response);
        buf_puts(&response, "{\"type\":\"response\",\"success\":true,\"id\":");
        json_dump(&response, json_get(j, "id"));
        buf_puts(&response, ",\"command\":"); json_dump(&response, json_get(j, "type"));
        buf_puts(&response, "}\n");
        assert(fputs(response.s, stdout) >= 0 && !fflush(stdout));
        if (type && !strcmp(type, "prompt")) {
            if (first_call) {
                f = fopen(getenv("UNII_TEST_COMPACTION_STARTED"), "wb");
                assert(f && fputc('1', f) != EOF && !fclose(f));
                gate(getenv("UNII_TEST_COMPACTION_GATE"), 1);
                first_call = 0;
            }
            fputs("{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
                  "\"content\":[{\"type\":\"text\",\"text\":\"note: long seed preserved\"}],"
                  "\"stopReason\":\"stop\"}}\n{\"type\":\"agent_settled\"}\n", stdout);
            assert(!fflush(stdout));
        }
        json_free(j);
    }
    buf_free(&input); buf_free(&response);
    return 0;
}

static void set_path(const char *name, const char *dir, const char *file)
{
    char *path;
    path = path_join(dir, file);
    assert(!setenv(name, path, 1));
    free(path);
}

static pid_t start(FILE **in, FILE **out, const char *project)
{
    int input[2], output[2];
    pid_t pid;
    char *extension, *provider;
    extension = path_join(project, "pi/unii.ts");
    provider = path_join(project, "tests/pi_steering_model.ts");
    assert(!pipe(input) && !pipe(output));
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        assert(!setpgid(0, 0));
        assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", extension, "--extension", provider,
               "--provider", "unii-steering", "--model", "deterministic",
               "--tools", "zoom,date", (char *)NULL);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    *in = fdopen(output[0], "r"); *out = fdopen(input[1], "w");
    assert(*in && *out);
    free(extension); free(provider);
    return pid;
}

static void send(FILE *out, unsigned long id, const char *type, const char *text)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, id);
    buf_puts(&b, ",\"type\":"); buf_json(&b, type);
    buf_puts(&b, ",\"message\":"); buf_json(&b, text);
    buf_puts(&b, "}\n");
    assert(fputs(b.s, out) >= 0 && !fflush(out));
    buf_free(&b);
}

static Json *receive(FILE *in)
{
    Buf b;
    Json *j;
    const char *type, *reason;
    buf_init(&b);
    assert(read_line(in, &b) == 1);
    j = json_parse(b.s); assert(j);
    type = json_str(json_get(j, "type"));
    reason = json_str(json_get(json_get(j, "message"), "stopReason"));
    if (type && (!strcmp(type, "extension_error") ||
        (reason && (!strcmp(reason, "error") || !strcmp(reason, "aborted"))))) {
        fprintf(stderr, "Pi steering failed: %s\n", b.s);
        assert(0);
    }
    buf_free(&b);
    return j;
}

static void response(FILE *in, unsigned long id, const char *disposition)
{
    Json *j;
    unsigned long got;
    const char *type, *actual;
    for (;;) {
        j = receive(in);
        type = json_str(json_get(j, "type"));
        if (type && !strcmp(type, "response") && json_ulong(json_get(j, "id"), &got) && got == id) {
            actual = json_str(json_get(json_get(j, "data"), "disposition"));
            assert(actual && !strcmp(actual, disposition));
            json_free(j);
            return;
        }
        json_free(j);
    }
}

static void event(FILE *in, const char *wanted, const char *role)
{
    Json *j;
    const char *type, *got;
    int match;
    for (;;) {
        j = receive(in);
        type = json_str(json_get(j, "type"));
        got = json_str(json_get(json_get(j, "message"), "role"));
        match = type && !strcmp(type, wanted) && (!role || (got && !strcmp(role, got)));
        json_free(j);
        if (match) return;
    }
}

static char *text(const Json *message)
{
    Buf b;
    const Json *p;
    const char *s;
    buf_init(&b);
    p = json_get(message, "content");
    assert(p && p->type == J_ARR);
    for (p = p->child; p; p = p->next) {
        s = json_str(json_get(p, "text"));
        if (s) buf_puts(&b, s);
    }
    return b.s;
}

static void contexts(const char *dir)
{
    char *path, *s, *end;
    const char *role;
    FILE *f;
    Buf b;
    Json *j, *p;
    unsigned long call;
    size_t count, users, assistants;
    path = path_join(dir, "provider.jsonl");
    f = fopen(path, "rb"); assert(f); free(path);
    count = 0; buf_init(&b);
    while (read_line(f, &b) == 1) {
        j = json_parse(b.s);
        assert(j && json_ulong(json_get(j, "call"), &call) && call == count);
        users = assistants = 0;
        p = json_get(j, "messages"); assert(p && p->type == J_ARR);
        for (p = p->child; p; p = p->next) {
            role = json_str(json_get(p, "role")); assert(role);
            if (!strcmp(role, "assistant")) assistants++;
            if (strcmp(role, "user")) continue;
            s = text(p);
            if (!users) {
                assert(!strncmp(s, "<chat>\n", 7));
                end = strstr(s, "</chat>\n"); assert(end);
                *end = 0;
                if (count < 2) {
                    assert(!strstr(s, first) && !strstr(s, waiting) && !strstr(s, active));
                    assert(strstr(end + 8, first));
                } else {
                    assert(strstr(s, first) && strstr(s, waiting) && strstr(s, active));
                    assert(strstr(end + 8, following));
                    assert(!strstr(end + 8, first) && !strstr(end + 8, waiting));
                }
            } else assert(!strcmp(s, users == 1 ? waiting : active));
            free(s); users++;
        }
        assert(users == (count == 0 ? 2U : count == 1 ? 3U : 1U));
        assert(assistants == (count == 1 ? 1U : 0U));
        json_free(j); count++;
    }
    assert(count == 3 && !fclose(f)); buf_free(&b);
}

static void saved_words(const char *dir)
{
    Store s;
    char *path, *message;
    unsigned long i;
    int a, b, c, d;
    path = path_join(dir, "chat");
    assert(store_open(&s, path)); free(path);
    a = b = c = d = 0;
    assert(s.count == 8);
    for (i = 0; i < s.count; i++) {
        message = store_message(&s, i); assert(message);
        if (!strcmp(message, first)) a++;
        if (!strcmp(message, waiting)) b++;
        if (!strcmp(message, active)) c++;
        if (!strcmp(message, following)) d++;
        free(message);
    }
    assert(a == 1 && b == 1 && c == 1 && d == 1);
    store_close(&s);
}

int main(int argc, char **argv)
{
    char project[4096], seed[1001], *path;
    FILE *in, *out, *f;
    Store s;
    int status;
    if (argc > 1 && !strcmp(argv[1], "--mode")) return compactor();
    assert(argc == 2 && getcwd(project, sizeof(project)) && !mkdirs(argv[1]));
    set_path("UNII_BIN", project, "unii");
    set_path("UNII_PI", project, "tests/pi_steering_test");
    set_path("UNII_DIR", argv[1], "chat");
    set_path("UNII_TEST_PROVIDER_LOG", argv[1], "provider.jsonl");
    set_path("UNII_TEST_COMPACTION_STARTED", argv[1], "compaction-started");
    set_path("UNII_TEST_COMPACTION_GATE", argv[1], "compaction-gate");
    set_path("UNII_TEST_REPLY_GATE", argv[1], "reply-gate");
    set_path("PI_CODING_AGENT_DIR", argv[1], "pi-config");
    assert(!setenv("PI_OFFLINE", "1", 1) && !unsetenv("UNII_INSTRUCTIONS"));
    assert(!mkfifo(getenv("UNII_TEST_COMPACTION_STARTED"), 0600));
    assert(!mkfifo(getenv("UNII_TEST_COMPACTION_GATE"), 0600));
    assert(!mkfifo(getenv("UNII_TEST_REPLY_GATE"), 0600));
    path = path_join(argv[1], "chat");
    assert(store_open(&s, path)); free(path);
    memset(seed, 'S', 1000); seed[1000] = 0;
    assert(store_append(&s, "note", seed, NULL, NULL)); store_close(&s);
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);
    alarm(45); running = start(&in, &out, project);
    send(out, 1, "prompt", first);
    f = fopen(getenv("UNII_TEST_COMPACTION_STARTED"), "rb");
    assert(f && fgetc(f) == '1' && !fclose(f));
    send(out, 2, "steer", waiting); response(in, 2, "queued");
    gate(getenv("UNII_TEST_COMPACTION_GATE"), 0);
    response(in, 1, "started");
    event(in, "message_start", "assistant");
    send(out, 3, "steer", active); response(in, 3, "queued");
    gate(getenv("UNII_TEST_REPLY_GATE"), 0);
    event(in, "agent_settled", NULL);
    send(out, 4, "prompt", following); response(in, 4, "started");
    event(in, "agent_settled", NULL);
    assert(!fclose(out));
    while (fgetc(in) != EOF) ;
    assert(!fclose(in) && waitpid(running, &status, 0) == running);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    contexts(argv[1]); saved_words(argv[1]);
    puts("live Pi steering: waiting inputs, active steering, fresh following turn, durable words OK");
    return 0;
}
