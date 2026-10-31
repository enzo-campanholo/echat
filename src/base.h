#ifndef UNII_BASE_H
#define UNII_BASE_H

#include <stddef.h>
#include <stdio.h>

/* Growable byte string; s is NULL or NUL-terminated. len excludes NUL. */
typedef struct Buf { char *s; size_t len, cap; } Buf;
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void buf_add(Buf *b, const void *s, size_t n); /* s must not point into b */
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, int c);
void buf_ulong(Buf *b, unsigned long n);
void buf_json(Buf *b, const char *s);
int read_line(FILE *f, Buf *b); /* 1: LF-terminated record; 0: EOF; -1: partial/error */
char *read_file(const char *path);
int write_all(int fd, const void *data, size_t n);
int sync_fd(int fd);
size_t utf8_prefix(const char *s, size_t n); /* complete codepoints in first n bytes */
int utf8_valid(const char *s);
char *clip_echo(const char *s, size_t chars); /* budget in code points, not bytes */
char *path_join(const char *a, const char *b);
int sync_parent(const char *path);
/* Sync directories along the resolved path, including existing ancestors. */
int mkdirs(const char *path);
/* One setup may create several paths before its shared barrier. Always finish,
 * even on failure (sync=0 discards). No acknowledgment before finish(sync=1).
 * External namespace renames/replacements must not race these operations. */
typedef struct DirSync DirSync;
int mkdirs_pending(const char *path, DirSync **pending); /* *pending starts NULL */
int finish_dirs(DirSync **pending, int sync); /* closes/frees and resets to NULL */

/* Strings are decoded UTF-8 without NUL. Duplicate object keys are rejected. */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct Json Json;
struct Json { int type; char *s; Json *child, *next; char *key; };
Json *json_parse(const char *s);
void json_free(Json *j);
Json *json_get(const Json *j, const char *key);
const char *json_str(const Json *j);
int json_ulong(const Json *j, unsigned long *n); /* 1: unsigned integer; 0: invalid/overflow */
void json_dump(Buf *b, const Json *j);

#endif
