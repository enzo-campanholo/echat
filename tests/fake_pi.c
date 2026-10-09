/* Deterministic RPC peer. It never calls a provider or executes a tool. */
#include "base.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void reply(const Json *j)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"type\":\"response\",\"success\":true,\"id\":");
    json_dump(&b, json_get(j, "id"));
    buf_puts(&b, ",\"command\":");
    json_dump(&b, json_get(j, "type"));
    buf_puts(&b, ",\"data\":{\"disposition\":\"started\"}}\n");
    fputs(b.s, stdout);
    fflush(stdout);
    buf_free(&b);
}

int main(void)
{
    Buf input, summary, record, socket_line;
    Json *j;
    const char *type, *message, *fail_path;
    const char *audit_path, *env;
    FILE *bridge;
    FILE *audit;
    int fd, fail;
    size_t k;
    bridge = fdopen(3, "r+");
    assert(bridge);
    setvbuf(bridge, NULL, _IONBF, 0);
    buf_init(&input);
    buf_init(&summary);
    buf_init(&record);
    buf_init(&socket_line);
    audit_path = getenv("UNII_TEST_COMPACTION_LOG");
    if (audit_path) {
        audit = fopen(audit_path, "ab");
        assert(audit);
        buf_puts(&record, "{\"system\":");
        buf_json(&record, getenv("UNII_SYSTEM_PROMPT"));
        buf_puts(&record, ",\"tools\":");
        env = getenv("UNII_TOOLS");
        buf_puts(&record, env ? env : "[]");
        buf_puts(&record, ",\"provider_tools\":");
        env = getenv("UNII_PROVIDER_TOOLS");
        buf_puts(&record, env ? env : "[]");
        buf_puts(&record, "}\n");
        assert(fwrite(record.s, 1, record.len, audit) == record.len);
        assert(!fclose(audit));
        buf_reset(&record);
    }
    while (read_line(stdin, &input) == 1) {
        j = json_parse(input.s);
        assert(j);
        type = json_str(json_get(j, "type"));
        reply(j);
        if (type && !strcmp(type, "prompt")) {
            message = json_str(json_get(j, "message"));
            assert(message);
            audit_path = getenv("UNII_TEST_COMPACTION_IMAGES");
            if (audit_path && json_get(j, "images")) {
                audit = fopen(audit_path, "ab");
                assert(audit);
                buf_reset(&record);
                buf_puts(&record, "{\"message\":");
                buf_json(&record, message);
                buf_puts(&record, ",\"images\":");
                json_dump(&record, json_get(j, "images"));
                buf_puts(&record, "}\n");
                assert(fwrite(record.s, 1, record.len, audit) == record.len);
                assert(!fclose(audit));
            }
            if (strncmp(message, "Too long:", 9)) {
                assert(!strncmp(message, "<chat>\n", 7));
                assert(strstr(message, "</chat>\n"));
                assert((size_t)(strstr(message, "</chat>\n") - message) + 8 <= 32000);
                assert(strstr(message, "Compaction:"));
                assert(!strstr(message, "(not summarized yet:"));
                assert(strstr(message, "<input>\n"));
            }
            fputs("{\"type\":\"prefix\",\"id\":\"gate\",\"key\":\"shared-test-prefix\"}\n", bridge);
            assert(read_line(bridge, &socket_line) == 1);
            assert(strstr(socket_line.s, "ready"));
            fputs("{\"type\":\"response_started\"}\n", bridge);
            fail = 0;
            fail_path = getenv("UNII_TEST_FAIL_MERGES");
            if (fail_path && access(fail_path, F_OK) == 0 &&
                strstr(message, "Compaction: merge lines"))
                fail = 1;
            fail_path = getenv("UNII_TEST_FAIL_ONCE");
            if (fail_path) {
                fd = open(fail_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) { close(fd); fail = 1; }
            }
            buf_reset(&summary);
            env = getenv("UNII_TEST_SUMMARY_BYTES");
            if (env) {
                assert(!strcmp(env, "500"));
                for (k = 0; k < 500; k++) buf_putc(&summary, 's');
            } else if (!strncmp(message, "Too long:", 9))
                buf_puts(&summary, "user: deterministic summary preserving the test's correction and result.");
            else
                for (k = 0; k < 600; k++) buf_putc(&summary, 'x');
            buf_reset(&record);
            buf_puts(&record, "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"never log this thought\"},{\"type\":\"text\",\"text\":");
            buf_json(&record, summary.s);
            buf_puts(&record, fail ? "}],\"stopReason\":\"error\",\"errorMessage\":\"test transient failure\"}}\n" : "}],\"stopReason\":\"stop\"}}\n");
            fputs(record.s, stdout);
            fputs("{\"type\":\"agent_settled\"}\n", stdout);
            fflush(stdout);
        }
        json_free(j);
    }
    fclose(bridge);
    buf_free(&input); buf_free(&summary); buf_free(&record); buf_free(&socket_line);
    return 0;
}
