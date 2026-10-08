#include "pi.h"

#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Capture {
    Buf log, text, thought;
    int calls;
} Capture;

static int capture_log(void *opaque, const char *kind, const char *text)
{
    Capture *c;
    c = opaque;
    buf_puts(&c->log, kind); buf_putc(&c->log, ':');
    buf_puts(&c->log, text); buf_putc(&c->log, '\n');
    c->calls++;
    return 0;
}

static void capture_display(void *opaque, const char *text, int thinking)
{
    Capture *c;
    c = opaque;
    buf_puts(thinking ? &c->thought : &c->text, text);
}

static void answer(const Json *request)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"type\":\"response\",\"command\":");
    json_dump(&b, json_get(request, "type"));
    buf_puts(&b, ",\"id\":"); json_dump(&b, json_get(request, "id"));
    buf_puts(&b, ",\"success\":true,\"data\":{\"cancelled\":false}}\n");
    assert(write_all(STDOUT_FILENO, b.s, b.len) == 0);
    buf_free(&b);
}

static int fake_pi(void)
{
    Buf line;
    Json *request;
    const char *type;
    int fresh, prompts;
    fresh = 0;
    prompts = 0;
    buf_init(&line);
    while (read_line(stdin, &line) == 1) {
        request = json_parse(line.s);
        assert(request != NULL);
        type = json_str(json_get(request, "type"));
        answer(request);
        if (!strcmp(type, "new_session")) fresh = 1;
        else if (!strcmp(type, "prompt")) {
            assert(fresh);
            fresh = 0;
            if (!prompts++) {
                assert(json_get(request, "images") != NULL);
                assert(json_get(request, "images")->type == J_ARR);
            } else assert(json_get(request, "images") == NULL);
            puts("{\"type\":\"message_update\",\"assistantMessageEvent\":{\"type\":\"thinking_delta\",\"delta\":\"secret reasoning\"}}");
            puts("{\"type\":\"message_update\",\"assistantMessageEvent\":{\"type\":\"text_delta\",\"delta\":\"Hello\"}}");
            puts("{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"secret reasoning\"},{\"type\":\"text\",\"text\":\"Hello\"},{\"type\":\"toolCall\",\"name\":\"zoom\",\"arguments\":{\"id\":0,\"n\":1}}],\"stopReason\":\"toolUse\"}}");
            puts("{\"type\":\"message_end\",\"message\":{\"role\":\"toolResult\",\"content\":[{\"type\":\"text\",\"text\":\"original message\"}]}}");
            puts("{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Done\"}],\"stopReason\":\"stop\"}}");
            puts("{\"type\":\"agent_end\",\"willRetry\":false}");
            puts("{\"type\":\"agent_settled\",\"aborted\":false}");
            fflush(stdout);
        }
        json_free(request);
    }
    buf_free(&line);
    return 0;
}

int main(int argc, char **argv)
{
    Pi p;
    PiConfig config;
    PiCallbacks callbacks;
    Capture capture;
    struct pollfd fds[2];
    int i, turn;
    if (argc > 1 && !strcmp(argv[1], "--mode")) return fake_pi();
    memset(&config, 0, sizeof(config));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&capture, 0, sizeof(capture));
    buf_init(&capture.log); buf_init(&capture.text); buf_init(&capture.thought);
    config.command = argv[0];
    config.extension = "unused-by-fake-pi";
    config.system = "Stable system";
    callbacks.opaque = &capture;
    callbacks.log = capture_log;
    callbacks.display = capture_display;
    assert(pi_start(&p, &config) == 0);
    for (turn = 0; turn < 2; turn++) {
        if (!turn) {
            assert(pi_prompt_images(&p, "<chat>\n</chat>", "Hello",
                "[{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"AA==\"}]", 1) == 0);
        } else assert(pi_prompt(&p, "<chat>\n</chat>", "Hello", 1) == 0);
        for (i = 0; i < 100 && !p.done; i++) {
            fds[0].fd = p.fd; fds[0].events = POLLIN;
            fds[1].fd = p.toolfd; fds[1].events = POLLIN;
            assert(poll(fds, 2, 100) >= 0);
            assert(pi_process(&p, &callbacks) == 0);
        }
        assert(p.done && !p.failed && p.first_response);
        assert(!strcmp(p.result.s, "Done"));
    }
    assert(capture.calls == 8);
    assert(!strstr(capture.log.s, "secret reasoning"));
    assert(strstr(capture.log.s, "unii:Hello\ntool:zoom {\"id\":0,\"n\":1}\necho:original message\nunii:Done\n"));
    assert(!strcmp(capture.text.s, "HelloHello"));
    assert(!strcmp(capture.thought.s, "secret reasoningsecret reasoning"));
    pi_stop(&p);
    buf_free(&capture.log); buf_free(&capture.text); buf_free(&capture.thought);
    puts("Pi fresh sessions, streaming and durable log passed");
    return 0;
}
