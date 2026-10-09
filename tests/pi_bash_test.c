#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Client { FILE *in, *out; pid_t pid; } Client;
static pid_t running;
static const char short_command[] = "printf '\\033['; sleep 1; "
    "printf '31mNATIVE_SHELL_RESULT\\033[0m\\000\\001\\002\\003\\004\\005\\006\\007\\010"
    "\\t\\013\\014\\r\\016\\017\\020\\021\\022\\023\\024\\025\\026\\027"
    "\\030\\031\\032\\034\\035\\036\\037\\n\\033]0;HIDDEN_TITLE\\007"
    "\\302\\23331mNEXT\\302\\2330m\\357\\277\\271\\357\\277\\272\\357\\277\\273\\n'; exit 7";
static const char short_output[] = "NATIVE_SHELL_RESULT\t\nNEXT\n";
static const char excluded_command[] = "printf 'EXCLUDED_SHELL_RESULT\\n'";
static const char long_command[] = "printf 'NATIVE_HEAD\\n'; "
    "awk 'BEGIN { for (i = 0; i < 40000; i++) printf \"\303\251\" }'; "
    "printf '\\nNATIVE_TAIL\\n'; exit 9";
static const char blocked_command[] = "printf SHOULD_NOT_RUN > \"$UNII_TEST_BLOCKED_MARKER\"";

static void timeout_handler(int sig)
{
    static const char text[] = "Pi native shell test exceeded 45 seconds\n";
    (void)sig;
    if (running > 0) kill(-running, SIGKILL);
    write(2, text, sizeof(text) - 1);
    _exit(124);
}

static void set_path(const char *name, const char *dir, const char *file)
{
    char *path;
    path = path_join(dir, file);
    assert(!setenv(name, path, 1));
    free(path);
}

static char *setup(const char *project, const char *root, const char *name, int fail)
{
    char *dir;
    dir = path_join(root, name); assert(!mkdirs(dir));
    set_path("UNII_BIN", project, fail ? "tests/pi_failure_test" : "unii");
    set_path("UNII_TEST_REAL_BIN", project, "unii");
    set_path("UNII_PI", project, "tests/fake_pi");
    set_path("UNII_DIR", dir, "chat");
    set_path("UNII_TEST_WORKER_LOG", dir, "provider.jsonl");
    set_path("UNII_TEST_BLOCKED_MARKER", dir, "executed");
    set_path("PI_CODING_AGENT_DIR", dir, "pi-config");
    assert(!unsetenv("UNII_TEST_FAIL_APPEND") && !unsetenv("UNII_TEST_FAIL_ONCE"));
    return dir;
}

static Client start(const char *project)
{
    Client c;
    int input[2], output[2];
    char *extension, *provider;
    extension = path_join(project, "pi/unii.ts");
    provider = path_join(project, "tests/pi_worker_model.ts");
    assert(!pipe(input) && !pipe(output));
    c.pid = fork(); assert(c.pid >= 0);
    if (!c.pid) {
        assert(!setpgid(0, 0));
        assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", extension, "--extension", provider,
               "--provider", "unii-worker-test", "--model", "deterministic",
               "--tools", "zoom,date", (char *)NULL);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    c.in = fdopen(output[0], "r"); c.out = fdopen(input[1], "w");
    assert(c.in && c.out); free(extension); free(provider);
    running = c.pid; alarm(45);
    return c;
}

static Json *receive(Client *c)
{
    Buf b;
    Json *j;
    buf_init(&b);
    assert(read_line(c->in, &b) == 1);
    j = json_parse(b.s); assert(j); buf_free(&b);
    return j;
}

static Json *request(Client *c, unsigned long id, const char *type,
                     const char *text, int excluded)
{
    Buf b;
    Json *j;
    unsigned long got;
    const char *kind;
    buf_init(&b);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, id);
    buf_puts(&b, ",\"type\":"); buf_json(&b, type);
    buf_puts(&b, !strcmp(type, "bash") ? ",\"command\":" : ",\"message\":");
    buf_json(&b, text);
    if (excluded) buf_puts(&b, ",\"excludeFromContext\":true");
    buf_puts(&b, "}\n");
    assert(fputs(b.s, c->out) >= 0 && !fflush(c->out)); buf_free(&b);
    for (;;) {
        j = receive(c);
        kind = json_str(json_get(j, "type"));
        if (kind && !strcmp(kind, "response") && json_ulong(json_get(j, "id"), &got) && got == id)
            return j;
        json_free(j);
    }
}

static void success(Json *j, int wanted)
{
    assert(!strcmp(json_get(j, "success")->s, wanted ? "true" : "false"));
}

static void settled(Client *c)
{
    Json *j;
    const char *type;
    int done;
    for (;;) {
        j = receive(c);
        type = json_str(json_get(j, "type"));
        done = type && !strcmp(type, "agent_settled");
        json_free(j);
        if (done) return;
    }
}

static void stop(Client *c)
{
    int status;
    assert(!fclose(c->out));
    while (fgetc(c->in) != EOF) ;
    assert(!fclose(c->in) && waitpid(c->pid, &status, 0) == c->pid);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static size_t lines(const char *dir)
{
    char *path;
    FILE *f;
    Buf b;
    size_t n;
    path = path_join(dir, "provider.jsonl");
    f = fopen(path, "rb"); free(path);
    if (!f) { assert(errno == ENOENT); return 0; }
    buf_init(&b); n = 0;
    while (read_line(f, &b) == 1) n++;
    buf_free(&b); assert(!fclose(f)); return n;
}

static void memory(const char *dir)
{
    Store s;
    char *path, *text, *end;
    FILE *f;
    Buf b, user;
    Json *j, *p, *content;
    unsigned long i;
    size_t k, chars;
    int commands, echoes, calls;
    path = path_join(dir, "chat"); assert(store_open(&s, path)); free(path);
    assert(s.count == 8);
    commands = echoes = 0;
    for (i = 0; i < s.count; i++) {
        text = store_message(&s, i); assert(text);
        assert(!strstr(text, "EXCLUDED_SHELL_RESULT"));
        if (!strcmp(text, short_command) || !strcmp(text, long_command)) {
            assert(!strcmp(s.messages[i].kind, "user")); commands++;
        }
        if (!strcmp(s.messages[i].kind, "echo")) {
            echoes++;
            assert(strstr(text, "cancelled=false"));
            if (strstr(text, "exit=7")) {
                end = strchr(text, '\n'); assert(end && !strcmp(end + 1, short_output));
            }
            else {
                assert(strstr(text, "exit=9") && strstr(text, "NATIVE_HEAD") && strstr(text, "NATIVE_TAIL"));
                assert(utf8_valid(text));
                chars = 0;
                for (k = 0; text[k]; k++) if (((unsigned char)text[k] & 192) != 128) chars++;
                assert(chars == 30000);
            }
        }
        free(text);
    }
    assert(commands == 2 && echoes == 2); store_close(&s);
    path = path_join(dir, "provider.jsonl"); f = fopen(path, "rb"); assert(f); free(path);
    buf_init(&b); buf_init(&user); calls = 0;
    while (read_line(f, &b) == 1) {
        j = json_parse(b.s); assert(j);
        p = json_get(j, "context"); assert(p && p->type == J_ARR);
        for (p = p->child; p && strcmp(json_str(json_get(p, "role")), "user"); p = p->next) ;
        assert(p && !p->next); buf_reset(&user);
        content = json_get(p, "content"); assert(content && content->type == J_ARR);
        for (content = content->child; content; content = content->next) {
            text = (char *)json_str(json_get(content, "text")); if (text) buf_puts(&user, text);
        }
        end = strstr(user.s, "</chat>\n"); assert(end); *end = 0;
        assert(strstr(user.s, short_command) && strstr(user.s, "NATIVE_SHELL_RESULT") && strstr(user.s, "exit=7"));
        assert(!strstr(user.s, "EXCLUDED_SHELL_RESULT"));
        assert(!strstr(end + 8, "NATIVE_SHELL_RESULT"));
        assert(strstr(end + 8, calls ? "AFTER_LONG_NATIVE_OUTPUT" : "AFTER_NATIVE_COMMAND"));
        json_free(j); calls++;
    }
    assert(calls == 2 && !fclose(f)); buf_free(&b); buf_free(&user);
}

static void blocked(const char *project, const char *root, int already_failed)
{
    Client c;
    Json *j;
    char *dir, *marker;
    dir = setup(project, root, already_failed ? "after-fatal" : "before-execution", 1);
    c = start(project);
    if (already_failed) {
        j = request(&c, 1, "prompt", "TRIGGER_ASSISTANT_APPEND_FAILURE", 0);
        success(j, 1); json_free(j); settled(&c);
    }
    j = request(&c, 2, "bash", blocked_command, 0); success(j, 0); json_free(j);
    marker = path_join(dir, "executed"); assert(access(marker, F_OK) < 0 && errno == ENOENT); free(marker);
    j = request(&c, 3, "prompt", "MUST_NOT_CONTINUE_WITHOUT_MEMORY", 0);
    success(j, 1); assert(!strcmp(json_str(json_get(json_get(j, "data"), "disposition")), "handled"));
    json_free(j); stop(&c);
    assert(lines(dir) == (already_failed ? 1U : 0U)); free(dir);
}

static void failed_result(const char *project, const char *root)
{
    static const char command[] = "printf DID_RUN > \"$UNII_TEST_BLOCKED_MARKER\"";
    static const char after[] = "printf SHOULD_NOT_RUN > \"$UNII_TEST_BLOCKED_MARKER.after\"";
    Client c;
    Json *j;
    Store s;
    char *dir, *marker, *chat, *text;
    dir = setup(project, root, "result-failure", 1);
    assert(!setenv("UNII_TEST_FAIL_APPEND", "2", 1));
    c = start(project);
    j = request(&c, 1, "bash", command, 0); success(j, 0); json_free(j);
    marker = path_join(dir, "executed"); assert(!access(marker, F_OK)); free(marker);
    j = request(&c, 2, "bash", after, 0); success(j, 0); json_free(j);
    marker = path_join(dir, "executed.after");
    assert(access(marker, F_OK) < 0 && errno == ENOENT); free(marker);
    j = request(&c, 3, "prompt", "MUST_NOT_CONTINUE_AFTER_LOST_RESULT", 0);
    success(j, 1); assert(!strcmp(json_str(json_get(json_get(j, "data"), "disposition")), "handled"));
    json_free(j); stop(&c); assert(!lines(dir));
    chat = path_join(dir, "chat"); assert(store_open(&s, chat)); free(chat);
    assert(s.count == 1 && !strcmp(s.messages[0].kind, "user"));
    text = store_message(&s, 0); assert(text && !strcmp(text, command)); free(text);
    store_close(&s); free(dir);
}

static void recovered(const char *project, const char *root)
{
    Client c;
    Json *j;
    Store s;
    char note[1001], *dir, *chat, *text;
    dir = setup(project, root, "recoverable-summary", 0);
    set_path("UNII_TEST_FAIL_ONCE", dir, "failed-once");
    chat = path_join(dir, "chat"); assert(store_open(&s, chat));
    memset(note, 'L', 1000); note[1000] = 0;
    assert(store_append(&s, "note", note, NULL, NULL)); store_close(&s);
    c = start(project);
    j = request(&c, 1, "prompt", "TRANSIENT_PREDECESSOR_FAILURE", 0);
    success(j, 1); assert(!strcmp(json_str(json_get(json_get(j, "data"), "disposition")), "handled"));
    json_free(j); assert(!lines(dir));
    j = request(&c, 2, "bash", "printf RECOVERED_NATIVE_COMMAND", 0); success(j, 1); json_free(j);
    j = request(&c, 3, "prompt", "AFTER_RECOVERED_NATIVE_COMMAND", 0); success(j, 1); json_free(j);
    settled(&c); stop(&c); assert(lines(dir) == 1);
    assert(store_open(&s, chat) && s.count == 6);
    text = store_message(&s, 1); assert(text && !strcmp(text, "TRANSIENT_PREDECESSOR_FAILURE")); free(text);
    text = store_message(&s, 2); assert(text && !strcmp(text, "printf RECOVERED_NATIVE_COMMAND")); free(text);
    text = store_message(&s, 3); assert(text && strstr(text, "exit=0") && strstr(text, "RECOVERED_NATIVE_COMMAND")); free(text);
    store_close(&s); free(chat); free(dir);
}

int main(int argc, char **argv)
{
    char project[4096], *dir;
    Client c;
    Json *j, *data;
    unsigned long code;
    const char *path;
    assert(argc == 2 && getcwd(project, sizeof(project)) && !mkdirs(argv[1]));
    assert(!setenv("PI_OFFLINE", "1", 1) && !unsetenv("UNII_INSTRUCTIONS"));
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);
    dir = setup(project, argv[1], "normal", 0); c = start(project);
    j = request(&c, 1, "bash", short_command, 0); success(j, 1);
    data = json_get(j, "data");
    assert(json_ulong(json_get(data, "exitCode"), &code) && code == 7);
    assert(!strcmp(json_str(json_get(data, "output")), short_output)); json_free(j);
    j = request(&c, 2, "bash", excluded_command, 1); success(j, 1); json_free(j);
    j = request(&c, 3, "prompt", "AFTER_NATIVE_COMMAND", 0); success(j, 1); json_free(j); settled(&c);
    j = request(&c, 4, "bash", long_command, 0); success(j, 1);
    data = json_get(j, "data");
    assert(!strcmp(json_get(data, "truncated")->s, "true"));
    path = json_str(json_get(data, "fullOutputPath")); assert(path && !unlink(path)); json_free(j);
    j = request(&c, 5, "prompt", "AFTER_LONG_NATIVE_OUTPUT", 0); success(j, 1); json_free(j); settled(&c);
    stop(&c); memory(dir); free(dir);
    blocked(project, argv[1], 0); blocked(project, argv[1], 1);
    failed_result(project, argv[1]); recovered(project, argv[1]);
    puts("live Pi native shell: durable commands/results, normalization, UTF-8 clipping, exclusions, failures/recovery OK");
    return 0;
}
