#ifndef UNII_STORE_H
#define UNII_STORE_H

#include "base.h"
#include <sys/types.h>
#include <limits.h>

/* The spec's numbers. Sizes are UTF-8 bytes unless noted. */
#define NODE_BYTES 512        /* a summary line */
#define NODE_SLACK 64         /* "a few bytes over" is kept; more is cut */
#define MESSAGE_BYTES 30000   /* longer text is split into several messages */
#define ECHO_CHARS 30000      /* tool output keeps head and tail: code points */
#define VIEW_HIGH 128000      /* the turn view merges once it passes this... */
#define VIEW_LOW 64000        /* ...down to this */
#define COMPACT_HIGH 32000    /* the compaction view, likewise */
#define COMPACT_LOW 16000
#define WORKERS 8             /* concurrent model calls */
#define ATTEMPTS 5            /* answers per node; the shortest is kept */
#define RETRIES 3             /* automatic retries of a failed node: 15, 60, 300 s */
/* Spec change C2: unsummarized messages before a turn follow the view whole,
 * in <recent>, when they fit; otherwise the turn waits. 0 restores the spec
 * (also at build time: -DTAIL_BYTES=0). */
#ifndef TAIL_BYTES
#define TAIL_BYTES 32000
#endif
#define ZOOM_HEAD 120         /* bytes of each child line in a logged zoom reference */
#define TREE_LEVELS (sizeof(unsigned long) * CHAR_BIT - 1)

enum { KIND_USER, KIND_UNII, KIND_TOOL, KIND_ECHO, KIND_WORK, KIND_NOTE, KINDS };
enum { NODE_IDLE, NODE_READY, NODE_FAILED, NODE_DEFERRED }; /* Node.queued */

/* Text stays on disk: an index entry names its record, which every read
 * re-validates. size is the text's length. */
typedef struct Ref { unsigned int l; unsigned long i; } Ref;
typedef struct Message {
    off_t offset;
    unsigned long size;
    unsigned int file; /* Store.files */
    unsigned char kind, has_images;
} Message;
typedef struct Node {
    off_t offset;
    unsigned int file, size; /* size 0: unbuilt (a built line is never empty) */
    unsigned char queued, busy;
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
    unsigned long generation; /* kept in .state for compatibility; unused */
    int journaled; /* the shrinking flag the last .state recorded */
    char main_day[17], tree_day[17]; /* newest daily file names */
    char error[256];
} Store;
typedef struct Entry { const char *kind, *text; const Json *images; } Entry;

/* Functions returning int return 0 on success and -1 on failure, with
 * s->error explaining it; close the store after a failed open.
 * store_open creates or opens a chat as its one writer. A missing view on an
 * existing log is an error: replay is never a replacement for it. Messages
 * logged after the saved view join it as one-message lines, verbatim. */
int store_open(Store *s, const char *dir);
/* Lock-free and read-only; works while a writer runs. A partial final line
 * is a write in progress, not corruption. */
int store_open_read(Store *s, const char *dir);
/* Raw records [from, from + count) of DIR to OUT, without the full index. */
int store_log(Store *s, const char *dir, unsigned long from, unsigned long count,
              FILE *out);
void store_close(Store *s);
int store_fail(Store *s, const char *what, const char *path);
int store_save_view(Store *s);
int store_save_file(Store *s, const char *name, const Buf *b); /* atomic */
/* Log entries in one write and one fsync. Long text splits into several
 * messages; echo text is clipped first; images go with the first part only.
 * Images are pi blocks {type:"image",data:base64,mimeType:"image/..."}. */
int store_append(Store *s, const Entry *e, size_t n, unsigned long *first,
                 unsigned long *number);
int store_kind_id(const char *kind); /* -1 if not a message kind */
const char *store_kind(Store *s, unsigned long i);
Json *store_record(Store *s, unsigned long i); /* validated; json_free it */
char *store_message(Store *s, unsigned long i);
char *store_images(Store *s, unsigned long i); /* JSON array; NULL if none or error */
char *store_date(Store *s, unsigned long i);
char *store_text(Store *s, Ref r); /* a built node's text */
Node *store_node(Store *s, Ref r); /* NULL if nonexistent or unbuilt */
Node *store_slot(Store *s, Ref r); /* allocate slot */
unsigned int store_file(Store *s, const char *path);
int store_build(Store *s, Ref r, const char *text); /* append once, fsync */
/* zoom(id, n, page) as the model reads it. *ref, if given, is a short line to
 * log instead: the tree already holds what zoom shows (spec change C4). */
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
/* A saved view: contiguous [l, i] pairs from message 0, parents built. */
int view_parse(Store *s, View *v, const Json *j);
void view_line(Buf *b, Ref r, const char *text); /* id+n|text, LF as space */
size_t view_measure(Store *s, View *v);
/* Bytes of the prefix rendering before END (as view_render), from sizes alone. */
size_t view_prefix(Store *s, View *v, unsigned long end, int built_only);
/* Render the view's prefix before END, opening a node that crosses it.
 * built_only stops at the first unbuilt line. NULL when the prefix exceeds
 * LIMIT (0: none), never a cut view; or when a read fails (s->broken). */
char *view_render(Store *s, View *v, unsigned long end, int built_only,
                  size_t limit);
/* Most-due built sibling pair. Age is measured from the pair's last message.
 * max_lines != 0 selects a line budget (used to verify rollback equivalence).
 * Otherwise enforce sawtooth hi/lo bytes, retaining an unfinished batch. */
int view_shrink(Store *s, View *v, size_t hi, size_t lo, size_t max_lines);

#endif
