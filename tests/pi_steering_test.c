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
static const char typed[] = "TYPED_WHILE_FLUSH_WAITS";
static pid_t running;
static int progress, restored;

static void timeout_handler(int sig)
{
    static const char text[] = "Pi steering test exceeded 45 seconds\n";
    (void)sig;
    if (running > 0) kill(-running, SIGKILL);
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(125);
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

static int add_note(Store *s, const char *text)
{
    Entry e;
    e.kind = "note";
    e.text = text;
    e.images = NULL;
    return !store_append(s, &e, 1, NULL, NULL);
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
    char *extension, *provider, *summary;
    extension = path_join(project, "pi/unii.ts");
    provider = path_join(project, "tests/pi_steering_model.ts");
    summary = path_join(project, "tests/pi_summary_model.ts");
    assert(!pipe(input) && !pipe(output));
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        assert(!setpgid(0, 0));
        assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", extension, "--extension", provider, "--extension", summary,
               "--provider", "unii-steering", "--model", "deterministic",
               "--tools", "zoom,date", (char *)NULL);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    *in = fdopen(output[0], "r"); *out = fdopen(input[1], "w");
    assert(*in && *out);
    free(extension); free(provider); free(summary);
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
    const char *type, *reason, *method, *status;
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
    /* Memory progress reaches RPC clients as setStatus records (B4). */
    method = json_str(json_get(j, "method"));
    status = json_str(json_get(j, "statusText"));
    if (type && !strcmp(type, "extension_ui_request") && method &&
        !strcmp(method, "setStatus") && status) {
        if (strstr(status, "summarizing "))
            progress++;
        else if (progress && !strncmp(status, "UniiChat \302\267 /", 13))
            restored++;
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
    assert(!store_open(&s, path)); free(path);
    a = b = c = d = 0;
    assert(s.count == 9);
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

/* A message typed while /unii flush waits supersedes the flush: the flush
 * ends with a notice and the message starts its turn, saved once. */
static void flush_superseded(const char *project, const char *root)
{
    char note[1001], *path, *message;
    const char *type, *method, *text, *kind, *disposition;
    FILE *in, *out, *f;
    Store s;
    Json *j;
    unsigned long got, i;
    int status, before, notice, started, saved;
    set_path("UNII_DIR", root, "flush-chat");
    set_path("UNII_TEST_PROVIDER_LOG", root, "flush-provider.jsonl");
    path = path_join(root, "flush-chat");
    assert(!store_open(&s, path));
    memset(note, 'F', 1000); note[1000] = 0; /* needs a summary call; fits <recent> */
    assert(add_note(&s, note)); store_close(&s);
    alarm(45); running = start(&in, &out, project);
    before = progress;
    send(out, 1, "prompt", "/unii flush");
    f = fopen(getenv("UNII_TEST_COMPACTION_STARTED"), "rb");
    assert(f && fgetc(f) == '1' && !fclose(f)); /* the call is held at its gate */
    while (progress == before) /* the engine reports the waiting flush */
        json_free(receive(in));
    send(out, 2, "prompt", typed);
    notice = started = 0;
    while (!notice || !started) {
        j = receive(in);
        type = json_str(json_get(j, "type"));
        method = json_str(json_get(j, "method"));
        text = json_str(json_get(j, "message"));
        if (type && !strcmp(type, "extension_ui_request") && method && !strcmp(method, "notify")) {
            assert(text && !strstr(text, "stopped") && !strstr(text, "did not save"));
            kind = json_str(json_get(j, "notifyType"));
            if (strstr(text, "flush superseded by a turn")) {
                assert(kind && !strcmp(kind, "error"));
                notice = 1;
            }
        }
        if (type && !strcmp(type, "response") && json_ulong(json_get(j, "id"), &got) && got == 2) {
            disposition = json_str(json_get(json_get(j, "data"), "disposition"));
            assert(disposition && !strcmp(disposition, "started"));
            started = 1;
        }
        json_free(j);
    }
    gate(getenv("UNII_TEST_COMPACTION_GATE"), 0);
    gate(getenv("UNII_TEST_REPLY_GATE"), 0);
    event(in, "agent_settled", NULL);
    assert(!fclose(out));
    while (fgetc(in) != EOF) ;
    assert(!fclose(in) && waitpid(running, &status, 0) == running);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!store_open(&s, path)); free(path);
    saved = 0;
    for (i = 0; i < s.count; i++) {
        message = store_message(&s, i); assert(message);
        saved += !strcmp(message, typed);
        free(message);
    }
    assert(saved == 1);
    store_close(&s);
}

int main(int argc, char **argv)
{
    char project[4096], seed[33001], *path;
    FILE *in, *out, *f;
    Store s;
    int status;
    assert(argc == 2 && getcwd(project, sizeof(project)) && !mkdirs(argv[1]));
    set_path("UNII_BIN", project, "unii");
    assert(!setenv("UNII_PROVIDER", "unii-summary", 1) && !setenv("UNII_MODEL", "deterministic", 1));
    set_path("UNII_DIR", argv[1], "chat");
    set_path("UNII_TEST_PROVIDER_LOG", argv[1], "provider.jsonl");
    set_path("UNII_TEST_COMPACTION_STARTED", argv[1], "compaction-started");
    set_path("UNII_TEST_COMPACTION_GATE", argv[1], "compaction-gate");
    set_path("UNII_TEST_REPLY_GATE", argv[1], "reply-gate");
    set_path("PI_CODING_AGENT_DIR", argv[1], "pi-config");
    assert(!setenv("PI_OFFLINE", "1", 1) && !unsetenv("UNII_INSTRUCTIONS"));
    assert(!setenv("UNII_TEST_SUMMARY_SHORT", "1", 1));
    assert(!mkfifo(getenv("UNII_TEST_COMPACTION_STARTED"), 0600));
    assert(!mkfifo(getenv("UNII_TEST_COMPACTION_GATE"), 0600));
    assert(!mkfifo(getenv("UNII_TEST_REPLY_GATE"), 0600));
    path = path_join(argv[1], "chat");
    assert(!store_open(&s, path)); free(path);
    /* Over TAIL_BYTES (two messages), so the turn waits for summaries. */
    memset(seed, 'S', 33000); seed[33000] = 0;
    assert(add_note(&s, seed)); store_close(&s);
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
    assert(progress >= 1 && restored >= 1);
    flush_superseded(project, argv[1]);
    puts("live Pi steering: waiting inputs, active steering, fresh following turn, durable words, input during flush OK");
    return 0;
}
