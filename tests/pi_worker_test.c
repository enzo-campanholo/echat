/* Real pi performs the engine's compaction calls in-process (B1). */
#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t running;

static void timeout_handler(int signal_number)
{
    static const char text[] = "pi compactions exceeded 60 seconds\n";
    (void)signal_number;
    if (running > 0) kill(-running, SIGKILL);
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(125);
    _exit(124);
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
    assert(setenv(name, path, 1) == 0);
    free(path);
}

static void start(FILE **input, FILE **output, const char *project)
{
    int in[2], out[2];
    char *extension, *summary;
    extension = path_join(project, "pi/unii.ts");
    summary = path_join(project, "tests/pi_summary_model.ts");
    assert(pipe(in) == 0 && pipe(out) == 0);
    running = fork();
    assert(running >= 0);
    if (!running) {
        assert(setpgid(0, 0) == 0);
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", extension, "--extension", summary,
               "--provider", "unii-summary", "--model", "deterministic",
               "--tools", "read,zoom,date", (char *)NULL);
        _exit(127);
    }
    free(extension); free(summary);
    close(in[0]); close(out[1]);
    *input = fdopen(out[0], "r");
    *output = fdopen(in[1], "w");
    assert(*input && *output);
}

/* Send a slash command and return its notify text. */
static char *command(FILE *input, FILE *output, unsigned long id, const char *text)
{
    Buf b, line;
    Json *j;
    const char *type, *method, *message;
    char *result;
    buf_init(&b); buf_init(&line);
    buf_puts(&b, "{\"id\":"); buf_ulong(&b, id);
    buf_puts(&b, ",\"type\":\"prompt\",\"message\":"); buf_json(&b, text);
    buf_puts(&b, "}\n");
    assert(fputs(b.s, output) >= 0 && fflush(output) == 0);
    result = NULL;
    while (!result && read_line(input, &line) == 1) {
        j = json_parse(line.s);
        assert(j);
        type = json_str(json_get(j, "type"));
        method = json_str(json_get(j, "method"));
        message = json_str(json_get(j, "message"));
        if (type && !strcmp(type, "extension_ui_request") && method &&
            !strcmp(method, "notify") && message && message[0] == '{')
            result = xstrdup(message);
        else if (type && !strcmp(type, "extension_ui_request") && method && !strcmp(method, "notify")) {
            fprintf(stderr, "unexpected notice: %s\n", line.s);
            assert(0);
        }
        json_free(j);
    }
    assert(result);
    buf_free(&b); buf_free(&line);
    return result;
}

static void compactions(const char *dir)
{
    char *path, *base, *current;
    const char *text;
    FILE *f;
    Buf line, encoded;
    Json *j;
    const Json *context, *message, *payload, *block, *system, *control;
    size_t total, gated, marked;
    path = path_join(dir, "compactions.jsonl");
    f = fopen(path, "rb");
    assert(f);
    free(path);
    total = gated = 0;
    base = NULL;
    buf_init(&line);
    while (read_line(f, &line) == 1) {
        j = json_parse(line.s);
        assert(j);
        assert(!strcmp(json_str(json_get(j, "reasoning")), "xhigh"));
        context = json_get(j, "context");
        assert(context && context->type == J_ARR);
        message = context->child;
        assert(message && !strcmp(json_str(json_get(message, "role")), "system"));
        assert(strstr(json_str(json_get(message, "content")), "You are Unii"));
        message = message->next; /* a fresh context: one user message */
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
    free(base);
    buf_free(&line);
    assert(fclose(f) == 0);
}

int main(int argc, char **argv)
{
    char project[4096], text[1001], *chat, *reply;
    FILE *input, *output;
    Store s;
    Json *j;
    unsigned long count;
    int k, status;
    assert(argc == 2 && getcwd(project, sizeof(project)));
    assert(mkdirs(argv[1]) == 0);
    set_path("UNII_BIN", project, "unii");
    set_path("UNII_DIR", argv[1], "chat");
    set_path("UNII_TEST_COMPACTION_LOG", argv[1], "compactions.jsonl");
    set_path("PI_CODING_AGENT_DIR", argv[1], "pi-config");
    assert(setenv("PI_OFFLINE", "1", 1) == 0 && !unsetenv("UNII_INSTRUCTIONS"));
    assert(!setenv("UNII_PROVIDER", "unii-summary", 1) && !setenv("UNII_MODEL", "deterministic", 1));
    assert(!setenv("UNII_TEST_SUMMARY_SHORT", "1", 1));
    chat = path_join(argv[1], "chat");
    assert(!store_open(&s, chat));
    memset(text, 'L', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    for (k = 0; k < 16; k++)
        assert(add_note(&s, text));
    store_close(&s);
    free(chat);
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);
    start(&input, &output, project);
    alarm(60);
    reply = command(input, output, 1, "/unii flush");
    j = json_parse(reply);
    assert(j && !strcmp(json_get(j, "ok")->s, "true"));
    json_free(j); free(reply);
    reply = command(input, output, 2, "/unii");
    j = json_parse(reply);
    assert(j && json_ulong(json_get(j, "messages"), &count) && count == 16);
    assert(json_ulong(json_get(j, "summarized"), &count) && count == 16);
    assert(json_ulong(json_get(j, "failed"), &count) && count == 0);
    json_free(j); free(reply);
    assert(fclose(output) == 0);
    while (fgetc(input) != EOF) ;
    assert(fclose(input) == 0);
    assert(waitpid(running, &status, 0) == running);
    running = 0; alarm(0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    compactions(argv[1]);
    puts("pi compactions: 31 direct calls, fresh context, shared system, cache marks OK");
    return 0;
}
