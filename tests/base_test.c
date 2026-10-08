#define _POSIX_C_SOURCE 200809L
#include "../src/base.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void invalid_json(void)
{
    static const char *bad[] = {
        "", " ", "[1,]", "{\"a\":1,}", "{a:1}", "[1 2]",
        "{\"a\":1,\"\\u0061\":2}", "{\"a\":1,\"a\":2}",
        "01", "-01", "-", "+1", ".1", "1.", "1e", "1e+",
        "NaN", "Infinity", "true false", "True", "[null]x",
        "\"\\x41\"", "\"\\u0000\"", "\"\\ud800\"",
        "\"\\udfff\"", "\"\\ud800\\u0061\"",
        "\"\\ud800\\ud800\"", "\"\\u123\"", "\"\\",
        "\"raw\nnewline\"", "\"unterminated", "[", "{", "true\v",
        "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"",
        "\"\xf0\x9f\"", "\"\xe2\"", "\"\x80\"", NULL
    };
    size_t i;
    for (i = 0; bad[i]; i++) {
        errno = 0;
        assert(json_parse(bad[i]) == NULL);
        assert(errno == EINVAL);
    }
}

static void json_roundtrip(void)
{
    const char *text;
    Json *j, *again, *item;
    Buf b, c;
    unsigned long n;
    text = " {\"text\":\"\\u00e9\\ud83d\\ude00\\n\\t\\b\\f\\r\\/\\\"\\\\\","
           "\"array\":[0,-0,1.25e-3,true,false,null,{},[]],\"id\":123} ";
    j = json_parse(text);
    assert(j && j->type == J_OBJ);
    assert(!strcmp(json_str(json_get(j, "text")),
                   "\xc3\xa9\xf0\x9f\x98\x80\n\t\b\f\r/\"\\"));
    assert(json_ulong(json_get(j, "id"), &n) == 1 && n == 123);
    assert(json_ulong(json_get(j, "text"), &n) == 0);
    assert(json_str(json_get(j, "id")) == NULL);
    assert(json_get(j, "missing") == NULL);
    assert(json_get(json_get(j, "id"), "id") == NULL);
    item = json_get(j, "array")->child;
    assert(json_ulong(item, &n) == 1 && n == 0);
    assert(json_ulong(item->next, &n) == 0);
    assert(json_ulong(item->next->next, &n) == 0);
    buf_init(&b);
    buf_init(&c);
    json_dump(&b, j);
    again = json_parse(b.s);
    assert(again);
    json_dump(&c, again);
    assert(!strcmp(b.s, c.s));
    json_free(again);
    json_free(j);
    buf_reset(&b);
    buf_ulong(&b, ULONG_MAX);
    j = json_parse(b.s);
    assert(j && json_ulong(j, &n) == 1 && n == ULONG_MAX);
    json_free(j);
    buf_putc(&b, '0');
    j = json_parse(b.s);
    assert(j && json_ulong(j, &n) == 0);
    json_free(j);
    buf_reset(&b);
    for (n = 0; n < 128; n++)
        buf_putc(&b, '[');
    buf_putc(&b, '0');
    for (n = 0; n < 128; n++)
        buf_putc(&b, ']');
    j = json_parse(b.s);
    assert(j);
    json_free(j);
    buf_reset(&b);
    for (n = 0; n < 129; n++)
        buf_putc(&b, '[');
    buf_putc(&b, '0');
    for (n = 0; n < 129; n++)
        buf_putc(&b, ']');
    assert(json_parse(b.s) == NULL);
    buf_free(&b);
    buf_free(&c);
}

static void utf8_and_clip(void)
{
    static const char *bad[] = {
        "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xff", "\xc2", "\xe2\x82",
        "\xf0\x9f\x98", "\xc2x", NULL
    };
    const char *mixed, *marker;
    char *clipped;
    size_t i, head_bytes, tail_bytes;
    Buf b;
    mixed = "a\xc3\xa9\xf0\x9f\x98\x80z";
    assert(utf8_valid(mixed));
    assert(utf8_valid(""));
    assert(utf8_valid("\xf4\x8f\xbf\xbf"));
    for (i = 0; bad[i]; i++) {
        assert(!utf8_valid(bad[i]));
        assert(clip_echo(bad[i]) == NULL);
    }
    assert(utf8_prefix(mixed, 0) == 0);
    assert(utf8_prefix(mixed, 1) == 1);
    assert(utf8_prefix(mixed, 2) == 1);
    assert(utf8_prefix(mixed, 3) == 3);
    for (i = 4; i < 7; i++)
        assert(utf8_prefix(mixed, i) == 3);
    assert(utf8_prefix(mixed, 7) == 7);
    assert(utf8_prefix(mixed, 99) == strlen(mixed));
    buf_init(&b);
    for (i = 0; i < 30000; i++)
        buf_puts(&b, "\xc3\xa9");
    clipped = clip_echo(b.s);
    assert(clipped && !strcmp(clipped, b.s));
    free(clipped);
    buf_reset(&b);
    for (i = 0; i < 15000; i++)
        buf_puts(&b, "\xf0\x9f\x98\x80");
    head_bytes = b.len;
    buf_puts(&b, "MISSING");
    for (i = 0; i < 15000; i++)
        buf_puts(&b, "\xc3\xa9");
    head_bytes = 14987 * 4;
    tail_bytes = 14987 * 2;
    clipped = clip_echo(b.s);
    assert(clipped && utf8_valid(clipped));
    assert(!memcmp(clipped, b.s, head_bytes));
    marker = strstr(clipped, "\n[... output clipped ...]\n");
    assert(marker == clipped + head_bytes);
    assert(!strcmp(marker + strlen("\n[... output clipped ...]\n"),
                   b.s + b.len - tail_bytes));
    assert(strstr(clipped, "MISSING") == NULL);
    free(clipped);
    buf_free(&b);
}

static void file_operations(void)
{
    char file[] = "/tmp/uniichat-base-XXXXXX";
    char dir[] = "/tmp/uniichat-dir-XXXXXX";
    char *p, *nested, *block;
    int fd;
    size_t i;
    FILE *f;
    Buf b;
    struct stat st;
    fd = mkstemp(file);
    assert(fd >= 0);
    buf_init(&b);
    for (i = 0; i < 200000; i++)
        buf_putc(&b, 'x');
    buf_puts(&b, "\n\npartial");
    assert(write_all(fd, b.s, b.len) == 0);
    assert(close(fd) == 0);
    p = read_file(file);
    assert(p && !strcmp(p, b.s));
    free(p);
    f = fopen(file, "r");
    assert(f);
    assert(read_line(f, &b) == 1 && b.len == 200000);
    assert(read_line(f, &b) == 1 && b.len == 0 && b.s && !*b.s);
    errno = 0;
    assert(read_line(f, &b) == -1 && errno == EINVAL);
    assert(!strcmp(b.s, "partial"));
    assert(read_line(f, &b) == 0 && b.len == 0);
    assert(fclose(f) == 0);
    f = fopen(file, "wb");
    assert(f && fwrite("a\0b\n", 1, 4, f) == 4);
    assert(fclose(f) == 0);
    assert(read_file(file) == NULL && errno == EINVAL);
    f = fopen(file, "rb");
    assert(f && read_line(f, &b) == -1 && errno == EINVAL);
    assert(fclose(f) == 0);
    assert(mkdirs(file) == -1 && errno == ENOTDIR);
    assert(unlink(file) == 0);
    fd = mkstemp(dir);
    assert(fd >= 0 && close(fd) == 0 && unlink(dir) == 0);
    nested = path_join(dir, "a/b/");
    assert(mkdirs(nested) == 0);
    assert(mkdirs(nested) == 0);
    assert(!stat(nested, &st) && S_ISDIR(st.st_mode));
    assert(rmdir(nested) == 0);
    free(nested);
    block = path_join(dir, "a");
    assert(rmdir(block) == 0);
    free(block);
    assert(rmdir(dir) == 0);
    assert(mkdirs("") == -1 && errno == EINVAL);
    assert(mkdirs("/") == 0);
    p = path_join("", "");
    assert(p && !*p);
    free(p);
    p = path_join("a/", "b");
    assert(!strcmp(p, "a/b"));
    free(p);
    buf_free(&b);
}

static void buffers(void)
{
    Buf b;
    size_t i;
    buf_init(&b);
    buf_add(&b, "", 0);
    assert(b.s && !b.len && !*b.s);
    buf_puts(&b, "self append");
    buf_add(&b, b.s, b.len);
    assert(!strcmp(b.s, "self appendself append"));
    buf_reset(&b);
    for (i = 0; i < 100000; i++)
        buf_putc(&b, (int)('a' + i % 26));
    assert(b.len == 100000 && b.s[b.len] == 0);
    buf_reset(&b);
    buf_json(&b, "\001\"\\\b\f\n\r\t\xc3\xa9");
    assert(!strcmp(b.s, "\"\\u0001\\\"\\\\\\b\\f\\n\\r\\t\xc3\xa9\""));
    buf_free(&b);
}

int main(void)
{
    invalid_json();
    json_roundtrip();
    utf8_and_clip();
    file_operations();
    buffers();
    puts("base: ok");
    return 0;
}
