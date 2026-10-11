#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char first_message[] = "FIRST_USER_RECORD_MUST_ONLY_APPEAR_IN_VIEW";
static const char second_message[] = "SECOND_FRESH_TURN";
static const char image_data[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
static pid_t running;

static void timeout_handler(int signal_number)
{
    static const char text[] = "live Pi test exceeded 30 seconds\n";
    (void)signal_number;
    if (running > 0)
        kill(-running, SIGKILL);
    if (write(2, text, sizeof(text) - 1) < 0)
        _exit(125);
    _exit(124);
}

static void set_path(const char *name, const char *dir, const char *file)
{
    char *path;
    path = path_join(dir, file);
    assert(setenv(name, path, 1) == 0);
    free(path);
}

static pid_t start(FILE **input, FILE **output, const char *project)
{
    int in[2], out[2];
    pid_t pid;
    char *frontend, *provider, *summary;
    frontend = path_join(project, "pi/unii.ts");
    provider = path_join(project, "tests/pi_model.ts");
    summary = path_join(project, "tests/pi_summary_model.ts");
    assert(pipe(in) == 0 && pipe(out) == 0);
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        assert(setpgid(0, 0) == 0);
        assert(dup2(in[0], 0) >= 0 && dup2(out[1], 1) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp("pi", "pi", "--mode", "rpc", "--no-session", "--offline",
               "--no-extensions", "--no-skills", "--no-prompt-templates",
               "--no-context-files", "--no-mcp", "--no-approve",
               "--extension", frontend, "--extension", provider, "--extension", summary,
               "--provider", "unii-test", "--model", "deterministic",
               "--tools", "read,zoom,date", (char *)NULL);
        perror("pi");
        _exit(127);
    }
    close(in[0]); close(out[1]);
    *input = fdopen(out[0], "r");
    *output = fdopen(in[1], "w");
    assert(*input && *output);
    free(frontend);
    free(provider); free(summary);
    return pid;
}

static void send_record(FILE *out, const char *type, const char *message, int image)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"type\":");
    buf_json(&b, type);
    if (message) {
        buf_puts(&b, ",\"message\":");
        buf_json(&b, message);
    } else
        buf_puts(&b, ",\"enabled\":false");
    if (image) { /* a client's extra field never reaches the engine */
        buf_puts(&b, ",\"images\":[{\"type\":\"image\",\"data\":");
        buf_json(&b, image_data);
        buf_puts(&b, ",\"mimeType\":\"image/png\",\"name\":\"pixel.png\"}]");
    }
    buf_puts(&b, "}\n");
    assert(fwrite(b.s, 1, b.len, out) == b.len && fflush(out) == 0);
    buf_free(&b);
}

/* /unii flush: memory catches up, so turn 2 sees summaries, not <recent>. */
static void flush(FILE *in, FILE *out)
{
    Buf b;
    Json *j;
    const char *type, *method, *message, *id;
    int answered, flushed;
    static const char record[] = "{\"type\":\"prompt\",\"id\":\"flush\",\"message\":\"/unii flush\"}\n";
    assert(fwrite(record, 1, sizeof(record) - 1, out) == sizeof(record) - 1 && fflush(out) == 0);
    answered = flushed = 0;
    buf_init(&b);
    while ((!answered || !flushed) && read_line(in, &b) == 1) {
        j = json_parse(b.s);
        assert(j);
        type = json_str(json_get(j, "type"));
        method = json_str(json_get(j, "method"));
        message = json_str(json_get(j, "message"));
        id = json_str(json_get(j, "id"));
        if (type && !strcmp(type, "response") && id && !strcmp(id, "flush"))
            answered = 1;
        if (method && !strcmp(method, "notify") && message && strstr(message, "\"ok\":true"))
            flushed = 1;
        json_free(j);
    }
    assert(answered && flushed);
    buf_free(&b);
}

static void finish_turn(FILE *in, int *thoughts, int *tools)
{
    Buf b;
    Json *j, *message, *p, *success;
    const char *type, *reason;
    int done;
    done = 0;
    buf_init(&b);
    while (read_line(in, &b) == 1) {
        j = json_parse(b.s);
        assert(j);
        type = json_str(json_get(j, "type"));
        assert(type);
        success = json_get(j, "success");
        if (success && success->type == J_BOOL && !strcmp(success->s, "false")) {
            fprintf(stderr, "Pi command failed: %s\n", b.s);
            assert(0);
        }
        if (!strcmp(type, "message_end")) {
            message = json_get(j, "message");
            reason = json_str(json_get(message, "stopReason"));
            if (reason && (!strcmp(reason, "error") || !strcmp(reason, "aborted"))) {
                fprintf(stderr, "Pi provider failed: %s\n", b.s);
                assert(0);
            }
            p = json_get(message, "content");
            for (p = p && p->type == J_ARR ? p->child : NULL; p; p = p->next) {
                type = json_str(json_get(p, "type"));
                if (type && !strcmp(type, "thinking"))
                    (*thoughts)++;
                if (type && !strcmp(type, "toolCall"))
                    (*tools)++;
            }
            type = json_str(json_get(j, "type"));
        }
        if (!strcmp(type, "agent_settled"))
            done = 1;
        json_free(j);
        if (done)
            break;
    }
    assert(done);
    buf_free(&b);
}

static char *dump(const Json *j)
{
    Buf b;
    buf_init(&b);
    json_dump(&b, j);
    return b.s;
}

static char *message_text(const Json *message)
{
    const Json *content, *p;
    const char *text;
    Buf b;
    buf_init(&b);
    content = json_get(message, "content");
    if (content && content->type == J_STR)
        buf_puts(&b, content->s);
    else {
        assert(content && content->type == J_ARR);
        for (p = content->child; p; p = p->next) {
            text = json_str(json_get(p, "text"));
            if (text)
                buf_puts(&b, text);
        }
    }
    return b.s;
}

static size_t image_blocks(const Json *content)
{
    const Json *p;
    const char *type, *data, *mime;
    size_t count;
    count = 0;
    assert(content && content->type == J_ARR);
    for (p = content->child; p; p = p->next) {
        type = json_str(json_get(p, "type"));
        if (type && !strcmp(type, "image")) {
            data = json_str(json_get(p, "data"));
            mime = json_str(json_get(p, "mimeType"));
            assert(data && !strcmp(data, image_data));
            assert(mime && !strcmp(mime, "image/png"));
            count++;
        }
    }
    return count;
}

static void contexts(const char *dir, char **system_out, char **tools_out,
                     char **provider_tools_out)
{
    char *path, *system, *tools, *provider_tools, *current, *text, *end, *mention;
    Json *j, *context, *user, *p;
    FILE *f;
    Buf b;
    unsigned long call;
    size_t count, users, assistants, results, result_images;
    path = path_join(dir, "provider.jsonl");
    f = fopen(path, "rb");
    assert(f);
    free(path);
    system = tools = provider_tools = NULL;
    count = 0;
    buf_init(&b);
    while (read_line(f, &b) == 1) {
        j = json_parse(b.s);
        assert(j && json_ulong(json_get(j, "call"), &call) && call == count);
        current = dump(json_get(j, "tools"));
        if (!tools)
            tools = xstrdup(current);
        assert(!strcmp(tools, current));
        free(current);
        current = dump(json_get(json_get(j, "payload"), "tools"));
        if (!provider_tools)
            provider_tools = xstrdup(current);
        assert(!strcmp(provider_tools, current));
        free(current);
        current = xstrdup(json_str(json_get(j, "system")));
        assert(strstr(current, "You are Unii"));
        if (!system)
            system = xstrdup(current);
        assert(!strcmp(system, current));
        free(current);
        context = json_get(j, "context");
        assert(context && context->type == J_ARR && context->child);
        p = context->child;
        assert(!strcmp(json_str(json_get(p, "role")), "system"));
        user = NULL;
        users = assistants = results = result_images = 0;
        for (p = p->next; p; p = p->next) {
            current = (char *)json_str(json_get(p, "role"));
            assert(current);
            if (!strcmp(current, "user")) {
                users++;
                user = p;
            } else if (!strcmp(current, "assistant"))
                assistants++;
            else if (!strcmp(current, "toolResult")) {
                results++;
                result_images += image_blocks(json_get(p, "content"));
            }
            else
                assert(0);
        }
        assert(users == 1 && user);
        text = message_text(user);
        assert(!strncmp(text, "<chat>\n", 7));
        end = strstr(text, "</chat>");
        assert(end);
        if (count >= 2) {
            assert(image_blocks(json_get(user, "content")) == 1);
            if (count == 2)
                assert(assistants == 0 && results == 0 && result_images == 0);
            else
                assert(assistants == 1 && results == 1 && result_images == 1);
            mention = strstr(text, first_message);
            assert(mention && mention < end);
            assert(!strstr(end, first_message));
            assert(strstr(end, second_message));
            assert(!strstr(end, "TURN_ONE_FINAL_REPLY"));
            assert(!strstr(text, "not summarized yet"));
        } else {
            assert(image_blocks(json_get(user, "content")) == 0 && result_images == 0);
            assert(strstr(end, first_message));
            assert(!strstr(text, second_message));
            if (count == 1)
                assert(assistants == 1 && results == 3);
        }
        free(text);
        json_free(j);
        count++;
    }
    assert(count == 4);
    assert(strstr(tools, "\"name\":\"zoom\"") && strstr(tools, "\"name\":\"date\"") &&
           strstr(tools, "\"name\":\"read\""));
    assert(fclose(f) == 0);
    buf_free(&b);
    *system_out = system;
    *tools_out = tools;
    *provider_tools_out = provider_tools;
}

static void compaction_contexts(const char *dir, const char *system,
                               const char *tools, const char *provider_tools)
{
    char *path, *current;
    FILE *f;
    Buf b;
    Json *j;
    size_t count;
    path = path_join(dir, "compactions.jsonl");
    f = fopen(path, "rb");
    assert(f);
    free(path);
    count = 0;
    buf_init(&b);
    while (read_line(f, &b) == 1) {
        j = json_parse(b.s);
        assert(j && !strcmp(json_str(json_get(j, "system")), system));
        current = dump(json_get(j, "tools"));
        assert(!strcmp(current, tools));
        free(current);
        current = dump(json_get(j, "provider_tools"));
        assert(!strcmp(current, provider_tools));
        free(current);
        json_free(j);
        count++;
    }
    assert(count > 0 && fclose(f) == 0);
    buf_free(&b);
}

static void image_compactions(const char *dir)
{
    char *path;
    FILE *f;
    Json *j;
    Buf b;
    const char *message;
    int user, echo;
    path = path_join(dir, "image-compactions.jsonl");
    f = fopen(path, "rb");
    assert(f);
    free(path);
    user = echo = 0;
    buf_init(&b);
    while (read_line(f, &b) == 1) {
        j = json_parse(b.s);
        assert(j && image_blocks(json_get(j, "images")) == 1);
        message = json_str(json_get(j, "message"));
        assert(message && strstr(message, second_message));
        if (strstr(message, "compress message 9 "))
            user++;
        else if (strstr(message, "compress message 11 "))
            echo++;
        else
            assert(0);
        json_free(j);
    }
    assert(user == 1 && echo == 0 && fclose(f) == 0); /* zoom echoes hold no images */
    buf_free(&b);
}

static void permanent_log(const char *dir)
{
    Store s;
    char *path, *text, *images, *date0;
    Ref r;
    Json *j;
    unsigned long i;
    unsigned int l;
    size_t k;
    int users, replies, calls, echoes, zoom, date, read;
    int opened, dated, read_result, image_messages;
    path = path_join(dir, "chat");
    if (store_open(&s, path) < 0) {
        fprintf(stderr, "live Pi chat could not reopen: %s\n", s.error);
        assert(0);
    }
    free(path);
    users = replies = calls = echoes = zoom = date = read = 0;
    date0 = store_date(&s, 0);
    assert(date0);
    opened = dated = read_result = 0;
    image_messages = 0;
    assert(s.count == 13);
    for (i = 0; i < s.count; i++) {
        text = store_message(&s, i);
        assert(text && !strstr(text, "NEVER_LOG_LIVE_THINKING") &&
               !strstr(text, "never log this thought"));
        if (i == 0)
            assert(!strcmp(text, first_message));
        if (i == 9)
            assert(!strcmp(text, second_message));
        assert(s.messages[i].has_images == (i == 9));
        if (s.messages[i].has_images) {
            images = store_images(&s, i);
            j = json_parse(images);
            assert(j && image_blocks(j) == 1);
            json_free(j);
            free(images);
            image_messages++;
        }
        if (!strcmp(store_kind(&s, i), "user"))
            users++;
        if (!strcmp(store_kind(&s, i), "unii"))
            replies++;
        if (!strcmp(store_kind(&s, i), "tool")) {
            calls++;
            if (!strncmp(text, "zoom ", 5)) zoom++;
            if (!strncmp(text, "date ", 5)) date++;
            if (!strncmp(text, "read ", 5)) read++;
        }
        if (!strcmp(store_kind(&s, i), "echo")) {
            echoes++;
            assert(*text);
            if (!strcmp(text, i == 5 ? "zoom {\"id\":0,\"n\":1,\"page\":0}: message 0 page 0/1, 42 bytes; not copied; zoom again to read" :
                        "zoom {\"id\":9,\"n\":1,\"page\":0}: message 9 page 0/1, 17 bytes; not copied; zoom again to read"))
                opened++;
            if (!strcmp(text, date0))
                dated++;
            if (strstr(text, "LIVE_FILE_CONTENT")) {
                read_result++;
                assert(strlen(text) > 512);
            }
        }
        free(text);
    }
    assert(users == 2 && replies == 3 && calls == 4 && echoes == 4);
    assert(zoom == 2 && date == 1 && read == 1);
    assert(opened == 2 && dated == 1 && read_result == 1);
    assert(image_messages == 1);
    for (l = 0; l < TREE_LEVELS; l++)
        for (k = 0; k < s.capacities[l]; k++)
            if (s.levels[l][k].size) {
                r.l = l;
                r.i = k;
                text = store_text(&s, r);
                assert(text && !strstr(text, "NEVER_LOG_LIVE_THINKING") &&
                       !strstr(text, "never log this thought"));
                free(text);
            }
    free(date0);
    store_close(&s);
}

int main(int argc, char **argv)
{
    char project[4096], *fixture, *system, *tools, *provider_tools;
    FILE *in, *out, *f;
    int status, thoughts, calls;
    size_t i;
    assert(argc == 2 && getcwd(project, sizeof(project)));
    assert(mkdirs(argv[1]) == 0);
    set_path("UNII_BIN", project, "unii");
    assert(!setenv("UNII_PROVIDER", "unii-summary", 1) && !setenv("UNII_MODEL", "deterministic", 1));
    set_path("UNII_DIR", argv[1], "chat");
    set_path("UNII_TEST_PROVIDER_LOG", argv[1], "provider.jsonl");
    set_path("UNII_TEST_COMPACTION_LOG", argv[1], "compactions.jsonl");
    set_path("UNII_TEST_COMPACTION_IMAGES", argv[1], "image-compactions.jsonl");
    set_path("UNII_TEST_READ", argv[1], "fixture.txt");
    set_path("PI_CODING_AGENT_DIR", argv[1], "pi-config");
    assert(setenv("PI_OFFLINE", "1", 1) == 0);
    assert(unsetenv("UNII_INSTRUCTIONS") == 0);
    fixture = path_join(argv[1], "fixture.txt");
    f = fopen(fixture, "wb");
    assert(f);
    fputs("LIVE_FILE_CONTENT\n", f);
    for (i = 0; i < 1200; i++)
        fputc('x', f);
    fputc('\n', f);
    assert(fclose(f) == 0);
    free(fixture);
    assert(signal(SIGALRM, timeout_handler) != SIG_ERR);
    running = start(&in, &out, project);
    alarm(30);
    send_record(out, "set_auto_compaction", NULL, 0);
    send_record(out, "set_auto_retry", NULL, 0);
    thoughts = calls = 0;
    send_record(out, "prompt", first_message, 0);
    finish_turn(in, &thoughts, &calls);
    flush(in, out);
    send_record(out, "prompt", second_message, 1);
    finish_turn(in, &thoughts, &calls);
    assert(thoughts == 1 && calls == 4);
    assert(fclose(out) == 0);
    while (fgetc(in) != EOF)
        ;
    assert(fclose(in) == 0);
    assert(waitpid(running, &status, 0) == running);
    running = 0;
    alarm(0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    contexts(argv[1], &system, &tools, &provider_tools);
    compaction_contexts(argv[1], system, tools, provider_tools);
    image_compactions(argv[1]);
    permanent_log(argv[1]);
    free(system); free(tools); free(provider_tools);
    puts("live Pi: fresh turns, shared context, real tools, durable images, unlogged thoughts OK");
    return 0;
}
