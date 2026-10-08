#include "pi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int nonblock(int fd)
{
    int flags;
    flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void fail(Pi *p, const char *text)
{
    p->failed = 1;
    if (!p->error.len) buf_puts(&p->error, text);
}

static char *request_id(Pi *p)
{
    Buf b;
    buf_init(&b);
    buf_ulong(&b, ++p->serial);
    return b.s;
}

static int enqueue(Pi *p, const char *record)
{
    if (p->writefd < 0) return -1;
    buf_puts(&p->out, record);
    buf_putc(&p->out, '\n');
    return pi_flush(p);
}

static int flush_buffer(Pi *p, int fd, Buf *b, size_t *position,
                        const char *error)
{
    ssize_t n;
    while (*position < b->len) {
        n = write(fd, b->s + *position, b->len - *position);
        if (n > 0) *position += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        else {
            fail(p, error);
            return -1;
        }
    }
    *position = 0;
    buf_reset(b);
    return 0;
}

int pi_flush(Pi *p)
{
    int a, b;
    a = flush_buffer(p, p->writefd, &p->out, &p->out_pos,
                     "Pi command pipe closed");
    b = flush_buffer(p, p->toolfd, &p->toolout, &p->toolout_pos,
                     "Pi memory socket closed");
    return a < 0 || b < 0 ? -1 : 0;
}

int pi_start(Pi *p, const PiConfig *c)
{
    int in[2], out[2], pair[2], n, saved;
    long maxfd;
    char *argv[32];
    pid_t pid;
    const char *command;

    memset(p, 0, sizeof(*p));
    p->pid = -1;
    p->fd = p->writefd = p->toolfd = -1;
    p->compaction = c->compaction;
    buf_init(&p->input); buf_init(&p->tools); buf_init(&p->out);
    buf_init(&p->toolout);
    buf_init(&p->result); buf_init(&p->error);
    if (!c->extension || !c->system) { errno = EINVAL; return -1; }
    if (pipe(in) < 0) return -1;
    if (pipe(out) < 0) { close(in[0]); close(in[1]); return -1; }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return -1;
    }
    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    command = c->command && *c->command ? c->command : "pi";
    n = 0;
    argv[n++] = (char *)command;
    argv[n++] = "--mode"; argv[n++] = "rpc";
    argv[n++] = "--no-session";
    argv[n++] = "--no-extensions";
    argv[n++] = "--no-skills";
    argv[n++] = "--no-prompt-templates";
    argv[n++] = "--no-approve";
    argv[n++] = "--extension"; argv[n++] = (char *)c->extension;
    argv[n++] = "--tools";
    argv[n++] = "read,bash,edit,write,grep,find,ls,zoom,date";
    argv[n++] = "--system-prompt"; argv[n++] = (char *)c->system;
    if (c->model && *c->model) {
        if (c->provider && *c->provider) {
            argv[n++] = "--provider"; argv[n++] = (char *)c->provider;
        }
        argv[n++] = "--model"; argv[n++] = (char *)c->model;
    }
    if (c->effort && *c->effort) {
        argv[n++] = "--thinking"; argv[n++] = (char *)c->effort;
    }
    argv[n] = NULL;
    pid = fork();
    if (pid == 0) {
        /* Move the socket above stdio before replacing descriptor 3. */
        saved = fcntl(pair[1], F_DUPFD, 10);
        if (saved < 0 || dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 ||
            dup2(saved, 3) < 0) _exit(126);
        for (n = 4; n < maxfd; n++) close(n);
        if (setenv("UNII_BRIDGE_FD", "3", 1) < 0 ||
            setenv("UNII_SYSTEM_PROMPT", c->system, 1) < 0 ||
            setenv("UNII_COMPACTION", c->compaction ? "1" : "0", 1) < 0 ||
            setenv("UNII_CACHE_GATE", c->cache_gate ? "1" : "0", 1) < 0 ||
            setenv("UNII_TOOLS", c->tools ? c->tools : "", 1) < 0 ||
            setenv("UNII_PROVIDER_TOOLS", c->provider_tools ? c->provider_tools : "", 1) < 0)
            _exit(126);
        execvp(command, argv);
        perror(command);
        _exit(127);
    }
    saved = errno;
    close(in[0]); close(out[1]); close(pair[1]);
    if (pid < 0) {
        close(in[1]); close(out[0]); close(pair[0]);
        errno = saved;
        return -1;
    }
    p->pid = pid; p->writefd = in[1]; p->fd = out[0]; p->toolfd = pair[0];
    if (nonblock(p->writefd) < 0 || nonblock(p->fd) < 0 ||
        nonblock(p->toolfd) < 0) { pi_stop(p); return -1; }
    signal(SIGPIPE, SIG_IGN);
    if (enqueue(p, "{\"type\":\"set_auto_compaction\",\"enabled\":false}") < 0 ||
        enqueue(p, "{\"type\":\"set_auto_retry\",\"enabled\":false}") < 0 ||
        enqueue(p, "{\"type\":\"set_steering_mode\",\"mode\":\"all\"}") < 0) {
        pi_stop(p); return -1;
    }
    return 0;
}

static char *prompt_record(Pi *p, const char *view, const char *task,
                           const char *type, const char *images)
{
    Buf b, message;
    char *id;
    buf_init(&b); buf_init(&message);
    id = request_id(p);
    if (view && *view) {
        buf_puts(&message, view);
        buf_puts(&message, "\n\n");
    }
    buf_puts(&message, task);
    buf_puts(&b, "{\"id\":"); buf_json(&b, id);
    buf_puts(&b, ",\"type\":"); buf_json(&b, type);
    buf_puts(&b, ",\"message\":"); buf_json(&b, message.s);
    if (images && *images) {
        buf_puts(&b, ",\"images\":"); buf_puts(&b, images);
    }
    buf_putc(&b, '}');
    free(id); buf_free(&message);
    return b.s;
}

int pi_prompt(Pi *p, const char *view, const char *task, int fresh)
{
    return pi_prompt_images(p, view, task, NULL, fresh);
}

int pi_prompt_images(Pi *p, const char *view, const char *task,
                     const char *images, int fresh)
{
    Buf b;
    char *record;
    int result;
    Json *j;
    if (p->busy || !task) { errno = EBUSY; return -1; }
    if (images && *images) {
        j = json_parse(images);
        if (!j || j->type != J_ARR) {
            json_free(j); errno = EINVAL; return -1;
        }
        json_free(j);
    }
    p->busy = 1; p->done = p->failed = p->first_response = 0;
    buf_reset(&p->result); buf_reset(&p->error);
    record = prompt_record(p, view, task, "prompt", images);
    if (!fresh) {
        result = enqueue(p, record);
        free(record);
        return result;
    }
    free(p->pending); p->pending = record;
    free(p->fresh_id); p->fresh_id = request_id(p);
    p->fresh_waiting = 1;
    buf_init(&b);
    buf_puts(&b, "{\"type\":\"new_session\",\"id\":");
    buf_json(&b, p->fresh_id); buf_putc(&b, '}');
    result = enqueue(p, b.s);
    buf_free(&b);
    return result;
}

int pi_steer(Pi *p, const char *message)
{
    char *record;
    int result;
    if (!p->busy || p->fresh_waiting) { errno = EBUSY; return -1; }
    record = prompt_record(p, NULL, message, "steer", NULL);
    result = enqueue(p, record);
    free(record);
    return result;
}

int pi_abort(Pi *p)
{
    return enqueue(p, "{\"type\":\"abort\"}");
}

static int same(const Json *j, const char *key, const char *s)
{
    const char *value;
    value = json_str(json_get(j, key));
    return value && !strcmp(value, s);
}

static int truth(const Json *j, const char *key)
{
    const Json *value;
    value = json_get(j, key);
    return value && value->type == J_BOOL && !strcmp(value->s, "true");
}

static void log_text(Pi *p, const PiCallbacks *cb, const char *kind,
                     const char *text)
{
    if (!p->compaction && text && *text && cb && cb->log &&
        cb->log(cb->opaque, kind, text) < 0) fail(p, "Cannot append Pi message");
}

static void tool_call(Pi *p, const PiCallbacks *cb, const char *name,
                      const Json *args)
{
    Buf b;
    if (p->compaction) { fail(p, "Compaction attempted to call a tool"); return; }
    buf_init(&b);
    buf_puts(&b, name ? name : "tool"); buf_putc(&b, ' ');
    json_dump(&b, args);
    log_text(p, cb, "tool", b.s);
    buf_free(&b);
}

static void message_end(Pi *p, const Json *message, const PiCallbacks *cb)
{
    const Json *content, *item;
    const char *text, *reason;
    Buf b;
    if (!message) return;
    content = json_get(message, "content");
    if (same(message, "role", "assistant")) {
        buf_reset(&p->result);
        for (item = content ? content->child : NULL; item; item = item->next) {
            if (same(item, "type", "text")) {
                text = json_str(json_get(item, "text"));
                if (text) {
                    if (p->result.len) buf_putc(&p->result, '\n');
                    buf_puts(&p->result, text);
                    log_text(p, cb, "unii", text);
                }
            } else if (same(item, "type", "toolCall")) {
                tool_call(p, cb, json_str(json_get(item, "name")),
                          json_get(item, "arguments"));
            }
        }
        text = json_str(json_get(message, "errorMessage"));
        reason = json_str(json_get(message, "stopReason"));
        if (reason && (!strcmp(reason, "error") || !strcmp(reason, "aborted")))
            fail(p, text && *text ? text : "Pi model request failed");
    } else if (same(message, "role", "toolResult")) {
        buf_init(&b);
        for (item = content ? content->child : NULL; item; item = item->next) {
            if (b.len) buf_putc(&b, '\n');
            if (same(item, "type", "text")) {
                text = json_str(json_get(item, "text"));
                if (text) buf_puts(&b, text);
            } else json_dump(&b, item);
        }
        log_text(p, cb, "echo", b.s);
        buf_free(&b);
    }
}

static void response(Pi *p, const Json *j)
{
    const Json *success, *data;
    const char *id, *error;
    success = json_get(j, "success");
    if (success && success->type == J_BOOL && !strcmp(success->s, "false")) {
        error = json_str(json_get(j, "error"));
        fail(p, error ? error : "Pi rejected a command");
        p->busy = 0; p->done = 1;
        return;
    }
    id = json_str(json_get(j, "id"));
    if (p->fresh_waiting && id && p->fresh_id && !strcmp(id, p->fresh_id)) {
        data = json_get(j, "data");
        if (truth(data, "cancelled")) {
            fail(p, "Pi canceled the fresh session");
            p->busy = 0; p->done = 1;
        } else if (p->pending) enqueue(p, p->pending);
        free(p->pending); p->pending = NULL;
        p->fresh_waiting = 0;
    }
    data = json_get(j, "data");
    if (same(j, "command", "prompt") && same(data, "disposition", "handled")) {
        fail(p, "Pi handled the input without starting a model request");
        p->busy = 0; p->done = 1;
    }
}

static void record(Pi *p, const Json *j, const PiCallbacks *cb)
{
    const Json *event, *result;
    const char *text;
    if (same(j, "type", "response")) response(p, j);
    else if (same(j, "type", "message_end"))
        message_end(p, json_get(j, "message"), cb);
    else if (same(j, "type", "message_update")) {
        p->first_response = 1;
        event = json_get(j, "assistantMessageEvent");
        if (!p->compaction && cb && cb->display &&
            (same(event, "type", "text_delta") || same(event, "type", "thinking_delta"))) {
            text = json_str(json_get(event, "delta"));
            if (text) cb->display(cb->opaque, text, same(event, "type", "thinking_delta"));
        }
    } else if (same(j, "type", "tool_execution_start") && json_get(j, "parentToolCallId")) {
        tool_call(p, cb, json_str(json_get(j, "toolName")), json_get(j, "args"));
    } else if (same(j, "type", "tool_execution_end") && json_get(j, "parentToolCallId")) {
        result = json_get(j, "result");
        /* Nested results have no transcript message_end. */
        if (result) {
            Buf b;
            buf_init(&b); json_dump(&b, json_get(result, "content"));
            log_text(p, cb, "echo", b.s); buf_free(&b);
        }
    } else if (same(j, "type", "extension_error")) {
        text = json_str(json_get(j, "error"));
        fail(p, text ? text : "Pi extension failed");
    } else if (same(j, "type", "agent_settled")) {
        p->busy = 0; p->done = 1;
        if (truth(j, "aborted")) fail(p, "Pi request aborted");
    }
}

static int bridge_reply(Pi *p, const char *id, const char *text,
                        const char *images)
{
    Buf b;
    int result;
    buf_init(&b);
    buf_puts(&b, "{\"id\":"); buf_json(&b, id);
    if (text) { buf_puts(&b, ",\"text\":"); buf_json(&b, text); }
    else buf_puts(&b, ",\"error\":\"Memory lookup failed\"");
    if (text && images && *images) {
        buf_puts(&b, ",\"images\":"); buf_puts(&b, images);
    }
    buf_puts(&b, "}\n");
    buf_add(&p->toolout, b.s, b.len);
    result = pi_flush(p);
    buf_free(&b);
    return result;
}

int pi_release_prefix(Pi *p)
{
    int result;
    if (!p->prefix_waiting || !p->prefix_id) return 0;
    result = bridge_reply(p, p->prefix_id, "ready", NULL);
    if (result < 0) fail(p, "Cannot release Pi cache prefix");
    p->prefix_waiting = 0;
    return result;
}

static void bridge(Pi *p, const Json *j, const PiCallbacks *cb)
{
    const char *id, *name, *key;
    char *text, *images;
    if (same(j, "type", "response_started")) p->first_response = 1;
    else if (same(j, "type", "prefix")) {
        id = json_str(json_get(j, "id"));
        key = json_str(json_get(j, "key"));
        if (!id || !key) { fail(p, "Invalid Pi cache barrier request"); return; }
        free(p->prefix_id); p->prefix_id = xstrdup(id);
        free(p->prefix_key); p->prefix_key = xstrdup(key);
        p->prefix_waiting = 1;
        p->first_response = 0;
    } else if (same(j, "type", "memory")) {
        id = json_str(json_get(j, "id"));
        name = json_str(json_get(j, "name"));
        if (!id || !name) { fail(p, "Invalid Pi memory request"); return; }
        text = images = NULL;
        if (cb && cb->memory) text = cb->memory(cb->opaque, name, json_get(j, "args"));
        if (text && cb && cb->memory_images)
            images = cb->memory_images(cb->opaque, name, json_get(j, "args"));
        if (bridge_reply(p, id, text, images) < 0) fail(p, "Cannot send Pi memory result");
        free(text);
        free(images);
    }
}

static int drain(Pi *p, int *fd, Buf *b, int is_bridge,
                 const PiCallbacks *cb)
{
    char data[16384], *lf;
    ssize_t n;
    size_t used;
    Json *j;
    if (*fd < 0) return 0;
    for (;;) {
        n = read(*fd, data, sizeof(data));
        if (n > 0) {
            buf_add(b, data, (size_t)n);
            while ((lf = memchr(b->s, '\n', b->len)) != NULL) {
                used = (size_t)(lf - b->s) + 1;
                *lf = 0;
                if (lf > b->s && lf[-1] == '\r') lf[-1] = 0;
                j = memchr(b->s, 0, used - 1) ? NULL : json_parse(b->s);
                if (!j) fail(p, "Pi sent invalid JSON");
                else {
                    if (is_bridge) bridge(p, j, cb);
                    else record(p, j, cb);
                    json_free(j);
                }
                memmove(b->s, b->s + used, b->len - used);
                b->len -= used; b->s[b->len] = 0;
            }
        } else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        else {
            close(*fd); *fd = -1;
            if (n < 0 || p->busy || b->len) {
                fail(p, "Pi exited before completing its request");
                p->busy = 0; p->done = 1;
                return -1;
            }
            return 0;
        }
    }
}

int pi_process(Pi *p, const PiCallbacks *cb)
{
    int a, b, c;
    a = drain(p, &p->fd, &p->input, 0, cb);
    b = drain(p, &p->toolfd, &p->tools, 1, cb);
    c = pi_flush(p);
    return a < 0 || b < 0 || c < 0 ? -1 : 0;
}

void pi_stop(Pi *p)
{
    int status, tries;
    struct timespec pause;
    pid_t result;
    if (p->writefd >= 0) close(p->writefd);
    p->writefd = -1;
    if (p->pid > 0) {
        pause.tv_sec = 0; pause.tv_nsec = 10000000;
        result = 0;
        for (tries = 0; tries < 20; tries++) {
            result = waitpid(p->pid, &status, WNOHANG);
            if (result != 0) break;
            nanosleep(&pause, NULL);
        }
        if (result == 0) {
            kill(p->pid, SIGTERM);
            for (tries = 0; tries < 20; tries++) {
                result = waitpid(p->pid, &status, WNOHANG);
                if (result != 0) break;
                nanosleep(&pause, NULL);
            }
            if (result == 0) {
                kill(p->pid, SIGKILL);
                while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    }
    if (p->fd >= 0) close(p->fd);
    if (p->toolfd >= 0) close(p->toolfd);
    free(p->prefix_key); free(p->prefix_id); free(p->fresh_id); free(p->pending);
    buf_free(&p->input); buf_free(&p->tools); buf_free(&p->out);
    buf_free(&p->toolout);
    buf_free(&p->result); buf_free(&p->error);
    p->pid = -1; p->fd = p->writefd = p->toolfd = -1;
    p->prefix_key = p->prefix_id = p->fresh_id = p->pending = NULL;
}
