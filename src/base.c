#define _POSIX_C_SOURCE 200809L
/* Darwin hides F_FULLFSYNC from strict POSIX builds. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "base.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JSON_DEPTH 128

static void oom(void)
{
    fputs("uniichat: out of memory\n", stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p;
    p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    n = strlen(s);
    p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

void buf_init(Buf *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->s);
    buf_init(b);
}

void buf_reset(Buf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = 0;
}

static void buf_reserve(Buf *b, size_t n)
{
    size_t cap;
    if (n <= b->cap)
        return;
    cap = b->cap ? b->cap : 64;
    while (cap < n) {
        if (cap > (size_t)-1 / 2) {
            cap = n;
            break;
        }
        cap *= 2;
    }
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

/* s must not point into b: growing b may move it. */
void buf_add(Buf *b, const void *s, size_t n)
{
    if (n > (size_t)-1 - b->len - 1)
        oom();
    buf_reserve(b, b->len + n + 1);
    if (n)
        memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_puts(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_putc(Buf *b, int c)
{
    unsigned char ch;
    ch = (unsigned char)c;
    buf_add(b, &ch, 1);
}

void buf_ulong(Buf *b, unsigned long n)
{
    char digits[sizeof(unsigned long) * CHAR_BIT + 1];
    size_t i;
    i = sizeof(digits);
    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    buf_add(b, digits + i, sizeof(digits) - i);
}

void buf_json(Buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char c;
    buf_putc(b, '"');
    while (*s) {
        c = (unsigned char)*s++;
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\f': buf_puts(b, "\\f"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 32) {
                buf_puts(b, "\\u00");
                buf_putc(b, hex[c >> 4]);
                buf_putc(b, hex[c & 15]);
            } else {
                buf_putc(b, c);
            }
        }
    }
    buf_putc(b, '"');
}

int read_line(FILE *f, Buf *b)
{
    char chunk[512];
    size_t n;
    int c;
    buf_reset(b);
    n = 0;
    while ((c = getc(f)) != EOF) {
        if (c == '\n') {
            buf_add(b, chunk, n);
            buf_add(b, "", 0);
            return 1;
        }
        if (!c) {
            errno = EINVAL;
            return -1;
        }
        chunk[n++] = (char)c;
        if (n == sizeof(chunk)) {
            buf_add(b, chunk, n);
            n = 0;
        }
    }
    buf_add(b, chunk, n);
    if (ferror(f)) {
        if (!errno)
            errno = EIO;
        return -1;
    }
    if (b->len) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

char *read_file(const char *path)
{
    FILE *f;
    Buf b;
    char chunk[8192];
    size_t n;
    int e;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    buf_init(&b);
    while ((n = fread(chunk, 1, sizeof(chunk), f)) != 0) {
        if (memchr(chunk, 0, n)) {
            fclose(f);
            buf_free(&b);
            errno = EINVAL;
            return NULL;
        }
        buf_add(&b, chunk, n);
    }
    e = ferror(f) ? (errno ? errno : EIO) : 0;
    if (fclose(f) && !e)
        e = errno;
    if (e) {
        buf_free(&b);
        errno = e;
        return NULL;
    }
    buf_add(&b, "", 0);
    return b.s;
}

int write_all(int fd, const void *data, size_t n)
{
    const char *p;
    ssize_t wrote;
    size_t chunk;
    p = data;
    while (n) {
        chunk = n;
        if (chunk > (size_t)SSIZE_MAX)
            chunk = (size_t)SSIZE_MAX;
        wrote = write(fd, p, chunk);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!wrote) {
            errno = EIO;
            return -1;
        }
        p += wrote;
        n -= (size_t)wrote;
    }
    return 0;
}

/* fsync is no barrier on macOS: only F_FULLFSYNC flushes the drive cache.
 * Filesystems without it (some network mounts) fall back to fsync.
 * UNII_PLAIN_FSYNC (fault-injection builds only) always calls fsync. */
int sync_fd(int fd)
{
#if defined(F_FULLFSYNC) && !defined(UNII_PLAIN_FSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

/* Return the width of one scalar value, or zero for malformed input. */
static size_t utf8_char(const char *s, size_t avail, unsigned long *value)
{
    const unsigned char *p;
    unsigned long c, min;
    size_t i, n;
    p = (const unsigned char *)s;
    if (!avail)
        return 0;
    c = p[0];
    if (c < 128) {
        if (value)
            *value = c;
        return 1;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        n = 2;
        min = 0x80;
        c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        n = 3;
        min = 0x800;
        c &= 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 4;
        min = 0x10000;
        c &= 7;
    } else {
        return 0;
    }
    if (n > avail)
        return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        c = (c << 6) | (p[i] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return 0;
    if (value)
        *value = c;
    return n;
}

size_t utf8_prefix(const char *s, size_t n)
{
    size_t i, width;
    i = 0;
    while (i < n && s[i]) {
        width = utf8_char(s + i, n - i, NULL);
        if (!width)
            break;
        i += width;
    }
    return i;
}

int utf8_valid(const char *s)
{
    size_t width;
    while (*s) {
        width = utf8_char(s, 4, NULL);
        if (!width)
            return 0;
        s += width;
    }
    return 1;
}

char *clip_echo(const char *s, size_t chars)
{
    static const char marker[] = "\n[... output clipped ...]\n";
    const char *p, *head, *tail;
    size_t width, count, skip, head_count, tail_count;
    Buf b;
    count = 0;
    head_count = (chars - (sizeof(marker) - 1)) / 2;
    tail_count = chars - (sizeof(marker) - 1) - head_count;
    head = s;
    for (p = s; *p; p += width) {
        width = utf8_char(p, 4, NULL);
        if (!width) {
            errno = EINVAL;
            return NULL;
        }
        count++;
        if (count == head_count)
            head = p + width;
    }
    if (count <= chars)
        return xstrdup(s);
    skip = count - tail_count;
    tail = s;
    while (skip--) {
        width = utf8_char(tail, 4, NULL);
        tail += width;
    }
    buf_init(&b);
    buf_add(&b, s, (size_t)(head - s));
    buf_puts(&b, marker);
    buf_puts(&b, tail);
    return b.s;
}

char *path_join(const char *a, const char *b)
{
    Buf out;
    buf_init(&out);
    buf_puts(&out, a);
    if (out.len && out.s[out.len - 1] != '/' && *b)
        buf_putc(&out, '/');
    buf_puts(&out, b);
    return out.s;
}

static int make_dir(const char *path)
{
    struct stat st;
    if (!mkdir(path, 0700))
        return 0;
    if (errno != EEXIST)
        return -1;
    if (stat(path, &st))
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int mkdirs(const char *path)
{
    char *copy, *p;
    int e;
    if (!*path) {
        errno = EINVAL;
        return -1;
    }
    copy = xstrdup(path);
    for (p = copy + 1; *p; p++) {
        if (*p == '/' && p[-1] != '/') {
            *p = 0;
            if (make_dir(copy)) {
                e = errno;
                free(copy);
                errno = e;
                return -1;
            }
            *p = '/';
        }
    }
    e = make_dir(copy) ? errno : 0;
    free(copy);
    if (e) {
        errno = e;
        return -1;
    }
    return 0;
}

typedef struct Parser { const char *p; int depth; } Parser;

static void space(Parser *p)
{
    while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')
        p->p++;
}

static int hex4(Parser *p, unsigned long *out)
{
    unsigned long n;
    int i, v;
    unsigned char c;
    n = 0;
    for (i = 0; i < 4; i++) {
        c = (unsigned char)*p->p;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return 0;
        n = n * 16 + (unsigned long)v;
        p->p++;
    }
    *out = n;
    return 1;
}

static void put_scalar(Buf *b, unsigned long c)
{
    if (c < 0x80) {
        buf_putc(b, (int)c);
    } else if (c < 0x800) {
        buf_putc(b, (int)(0xc0 | (c >> 6)));
        buf_putc(b, (int)(0x80 | (c & 63)));
    } else if (c < 0x10000) {
        buf_putc(b, (int)(0xe0 | (c >> 12)));
        buf_putc(b, (int)(0x80 | ((c >> 6) & 63)));
        buf_putc(b, (int)(0x80 | (c & 63)));
    } else {
        buf_putc(b, (int)(0xf0 | (c >> 18)));
        buf_putc(b, (int)(0x80 | ((c >> 12) & 63)));
        buf_putc(b, (int)(0x80 | ((c >> 6) & 63)));
        buf_putc(b, (int)(0x80 | (c & 63)));
    }
}

static char *string(Parser *p)
{
    Buf b;
    unsigned long c, low;
    const char *run;
    size_t width;
    int ch;
    if (*p->p != '"')
        return NULL;
    p->p++;
    buf_init(&b);
    for (;;) {
        /* Copy a run of plain bytes at once, validating UTF-8 as it goes. */
        for (run = p->p; (unsigned char)*p->p >= 32 && *p->p != '"' && *p->p != '\\';) {
            if ((unsigned char)*p->p < 128) {
                p->p++;
                continue;
            }
            width = utf8_char(p->p, 4, NULL);
            if (!width)
                goto bad;
            p->p += width;
        }
        buf_add(&b, run, (size_t)(p->p - run));
        if (*p->p == '"')
            break;
        if (*p->p != '\\')
            goto bad; /* NUL or a control character */
        p->p++;
        ch = (unsigned char)*p->p;
        if (!ch)
            goto bad;
        p->p++;
        switch (ch) {
        case '"': case '\\': case '/': buf_putc(&b, ch); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u':
            if (!hex4(p, &c) || !c)
                goto bad;
            if (c >= 0xd800 && c <= 0xdbff) {
                if (*p->p != '\\')
                    goto bad;
                p->p++;
                if (*p->p != 'u')
                    goto bad;
                p->p++;
                if (!hex4(p, &low) || low < 0xdc00 || low > 0xdfff)
                    goto bad;
                c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
            } else if (c >= 0xdc00 && c <= 0xdfff) {
                goto bad;
            }
            put_scalar(&b, c);
            break;
        default: goto bad;
        }
    }
    p->p++;
    buf_add(&b, "", 0);
    return b.s;
bad:
    buf_free(&b);
    return NULL;
}

static Json *new_json(int type)
{
    Json *j;
    j = xmalloc(sizeof(*j));
    j->type = type;
    j->s = j->key = NULL;
    j->child = j->next = NULL;
    return j;
}

void json_free(Json *j)
{
    Json *next;
    while (j) {
        next = j->next;
        json_free(j->child);
        free(j->s);
        free(j->key);
        free(j);
        j = next;
    }
}

static Json *value(Parser *p);

static Json *container(Parser *p, int type)
{
    Json *j, *item, *old;
    Json **end;
    char *key;
    int close;
    if (p->depth >= JSON_DEPTH)
        return NULL;
    p->depth++;
    j = new_json(type);
    close = type == J_ARR ? ']' : '}';
    p->p++;
    space(p);
    end = &j->child;
    if (*p->p == close) {
        p->p++;
        p->depth--;
        return j;
    }
    for (;;) {
        key = NULL;
        if (type == J_OBJ) {
            key = string(p);
            if (!key)
                goto bad;
            for (old = j->child; old; old = old->next) {
                if (!strcmp(old->key, key)) {
                    free(key);
                    goto bad;
                }
            }
            space(p);
            if (*p->p != ':') {
                free(key);
                goto bad;
            }
            p->p++;
            space(p);
        }
        item = value(p);
        if (!item) {
            free(key);
            goto bad;
        }
        item->key = key;
        *end = item;
        end = &item->next;
        space(p);
        if (*p->p == close) {
            p->p++;
            p->depth--;
            return j;
        }
        if (*p->p != ',')
            goto bad;
        p->p++;
        space(p);
    }
bad:
    p->depth--;
    json_free(j);
    return NULL;
}

static int digit(int c)
{
    return c >= '0' && c <= '9';
}

static Json *number(Parser *p)
{
    const char *start;
    Json *j;
    size_t n;
    start = p->p;
    if (*p->p == '-')
        p->p++;
    if (*p->p == '0') {
        p->p++;
    } else if (*p->p >= '1' && *p->p <= '9') {
        do { p->p++; } while (digit((unsigned char)*p->p));
    } else {
        return NULL;
    }
    if (*p->p == '.') {
        p->p++;
        if (!digit((unsigned char)*p->p))
            return NULL;
        do { p->p++; } while (digit((unsigned char)*p->p));
    }
    if (*p->p == 'e' || *p->p == 'E') {
        p->p++;
        if (*p->p == '+' || *p->p == '-')
            p->p++;
        if (!digit((unsigned char)*p->p))
            return NULL;
        do { p->p++; } while (digit((unsigned char)*p->p));
    }
    j = new_json(J_NUM);
    n = (size_t)(p->p - start);
    j->s = xmalloc(n + 1);
    memcpy(j->s, start, n);
    j->s[n] = 0;
    return j;
}

static Json *value(Parser *p)
{
    Json *j;
    int type;
    const char *literal;
    size_t len;
    if (*p->p == '[' || *p->p == '{')
        return container(p, *p->p == '[' ? J_ARR : J_OBJ);
    if (*p->p == '"') {
        j = new_json(J_STR);
        j->s = string(p);
        if (!j->s) {
            json_free(j);
            return NULL;
        }
        return j;
    }
    if (*p->p == '-' || digit((unsigned char)*p->p))
        return number(p);
    if (*p->p == 't') {
        literal = "true";
        type = J_BOOL;
    } else if (*p->p == 'f') {
        literal = "false";
        type = J_BOOL;
    } else if (*p->p == 'n') {
        literal = "null";
        type = J_NULL;
    } else {
        return NULL;
    }
    len = strlen(literal);
    if (strncmp(p->p, literal, len))
        return NULL;
    p->p += len;
    j = new_json(type);
    if (type == J_BOOL)
        j->s = xstrdup(literal);
    return j;
}

Json *json_parse(const char *s)
{
    Parser p;
    Json *j;
    if (!s) {
        errno = EINVAL;
        return NULL;
    }
    p.p = s;
    p.depth = 0;
    space(&p);
    j = value(&p);
    if (j) {
        space(&p);
        if (!*p.p)
            return j;
        json_free(j);
    }
    errno = EINVAL;
    return NULL;
}

Json *json_get(const Json *j, const char *key)
{
    Json *item;
    if (!j || j->type != J_OBJ)
        return NULL;
    for (item = j->child; item; item = item->next) {
        if (!strcmp(item->key, key))
            return item;
    }
    return NULL;
}

const char *json_str(const Json *j)
{
    return j && j->type == J_STR ? j->s : NULL;
}

int json_ulong(const Json *j, unsigned long *n)
{
    const char *p;
    unsigned long out, d;
    if (!j || j->type != J_NUM || !j->s || !*j->s)
        return 0;
    out = 0;
    for (p = j->s; *p; p++) {
        if (!digit((unsigned char)*p))
            return 0;
        d = (unsigned long)(*p - '0');
        if (out > (ULONG_MAX - d) / 10)
            return 0;
        out = out * 10 + d;
    }
    *n = out;
    return 1;
}

void json_dump(Buf *b, const Json *j)
{
    const Json *item;
    int comma;
    if (!j || j->type == J_NULL) {
        buf_puts(b, "null");
    } else if (j->type == J_STR) {
        buf_json(b, j->s);
    } else if (j->type == J_NUM || j->type == J_BOOL) {
        buf_puts(b, j->s);
    } else {
        buf_putc(b, j->type == J_ARR ? '[' : '{');
        comma = 0;
        for (item = j->child; item; item = item->next) {
            if (comma)
                buf_putc(b, ',');
            comma = 1;
            if (j->type == J_OBJ) {
                buf_json(b, item->key);
                buf_putc(b, ':');
            }
            json_dump(b, item);
        }
        buf_putc(b, j->type == J_ARR ? ']' : '}');
    }
}
