#ifndef UNII_STORE_H
#define UNII_STORE_H

#include "base.h"
#include <sys/types.h>
#include <limits.h>

/* Sizes are UTF-8 bytes except ECHO_CHARS. */
#define NODE_BYTES 512
#define NODE_SLACK 64         /* retain small overruns; cut larger ones */
#define MESSAGE_BYTES 30000
#define ECHO_CHARS 30000
#define VIEW_HIGH 128000
#define VIEW_LOW 64000
#define COMPACT_HIGH 32000
#define COMPACT_LOW 16000
#define WORKERS 8
#define ATTEMPTS 5
#define RETRIES 3
/* Set to 0 to wait for all earlier summaries instead of sending <recent>. */
#ifndef TAIL_BYTES
#define TAIL_BYTES 32000
#endif
#define ZOOM_HEAD 120         /* child excerpt in a zoom log reference */
#define TREE_LEVELS (sizeof(unsigned long) * CHAR_BIT - 1)

enum { KIND_USER, KIND_UNII, KIND_TOOL, KIND_ECHO, KIND_WORK, KIND_NOTE, KINDS };
enum { NODE_IDLE, NODE_READY, NODE_FAILED, NODE_DEFERRED }; /* Node.queued */

/* Indexes retain disk locations and record fingerprints, not text. */
typedef struct Ref { unsigned int l; unsigned long i; } Ref;
typedef struct Message {
    off_t offset;
    unsigned long size;
    unsigned int file; /* Store.files */
    unsigned char kind, has_images;
    unsigned char fingerprint[32];
} Message;
typedef struct Node {
    off_t offset;
    unsigned int file, size; /* size 0: unbuilt */
    unsigned char queued, busy, fingerprinted;
    unsigned char fingerprint[32];
} Node;
typedef struct View { Ref *v; size_t n, cap, bytes; int shrinking; } View;
typedef struct Store {
    char *dir;
    int lockfd, broken; /* broken: storage failed; stop using this store */
    char **files; /* daily log paths, interned */
    unsigned int nfiles, fcap;
    Message *messages;
    unsigned long count;
    size_t mcap;
    Node *levels[TREE_LEVELS];
    size_t capacities[TREE_LEVELS];
    View view;
    unsigned long generation; /* .state compatibility; not used for recovery */
    int journaled; /* last shrinking flag written to .state */
    char main_day[17], tree_day[17]; /* newest daily file names */
    char error[256];
} Store;
typedef struct Entry { const char *kind, *text; const Json *images; } Entry;

/* Open and write operations return 0 on success, -1 with s->error on failure.
 * Call store_close even after a failed open. */
int store_open(Store *s, const char *dir);
/* No writer lock; a partial final line is treated as a write in progress. */
int store_open_read(Store *s, const char *dir);
/* Stream [from, from + count) without building an index. */
int store_log(Store *s, const char *dir, unsigned long from, unsigned long count,
              FILE *out);
void store_close(Store *s);
int store_fail(Store *s, const char *what, const char *path);
int store_save_view(Store *s);
int store_save_file(Store *s, const char *name, const Buf *b); /* atomic */
/* One batch per sync. Split text at MESSAGE_BYTES, except clipped echoes;
 * only the first part carries images. */
int store_append(Store *s, const Entry *e, size_t n, unsigned long *first,
                 unsigned long *number);
int store_kind_id(const char *kind); /* -1 if not a message kind */
const char *store_kind(Store *s, unsigned long i);
/* Returned records and strings belong to the caller. */
Json *store_record(Store *s, unsigned long i);
char *store_message(Store *s, unsigned long i);
char *store_images(Store *s, unsigned long i); /* JSON array; NULL if none or error */
char *store_date(Store *s, unsigned long i);
char *store_text(Store *s, Ref r);
Node *store_node(Store *s, Ref r); /* NULL if nonexistent or unbuilt */
Node *store_slot(Store *s, Ref r);
unsigned int store_file(Store *s, const char *path);
int store_build(Store *s, Ref r, const char *text);
/* On success, *ref receives an allocated log reference, if requested. */
char *store_zoom(Store *s, unsigned long id, unsigned long n,
                 unsigned long page, char **ref);
int store_valid_ref(Store *s, Ref r);
unsigned long ref_start(Ref r);
unsigned long ref_span(Ref r);

void view_init(View *v);
void view_free(View *v);
void view_add(View *v, Ref r);
void view_copy(View *to, const View *from);
void view_dump(Buf *b, const View *v);
/* Require contiguous coverage from message 0 and built parents. */
int view_parse(Store *s, View *v, const Json *j);
void view_line(Buf *b, Ref r, const char *text); /* id+n|text, LF as space */
size_t view_measure(Store *s, View *v);
/* Same byte count as view_render, without reading text. */
size_t view_prefix(Store *s, View *v, unsigned long end, int built_only);
/* Render before end; built_only stops at the first unbuilt line. Return NULL
 * if limit is exceeded (0: unlimited) or a read fails (s->broken). */
char *view_render(Store *s, View *v, unsigned long end, int built_only,
                  size_t limit);
/* Merge built sibling pairs by age. A batch starts above hi and continues
 * toward lo as parents become available. Nonzero max_lines uses a line budget. */
int view_shrink(Store *s, View *v, size_t hi, size_t lo, size_t max_lines);

#endif
