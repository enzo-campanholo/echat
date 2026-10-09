#ifndef UNII_STORE_H
#define UNII_STORE_H

#include "base.h"
#include <sys/types.h>
#include <limits.h>

#define NODE_BYTES 512
#define MESSAGE_BYTES 30000
#define TREE_LEVELS (sizeof(unsigned long) * CHAR_BIT - 1)

typedef struct Ref { unsigned int l; unsigned long i; } Ref;
typedef struct Message {
    char *path;
    off_t offset;
    size_t size;
    char *kind;
    char *date;
    int has_images;
} Message;
typedef struct Node { char *text; size_t size; int queued; int busy; } Node;
typedef struct View { Ref *v; size_t n, cap, bytes; int shrinking; } View;
typedef struct Store {
    char *dir;
    int lockfd, broken; /* storage failure; callers must stop using this store */
    Message *messages;
    unsigned long count;
    size_t mcap;
    Node *levels[TREE_LEVELS];
    size_t capacities[TREE_LEVELS];
    View view;
    unsigned long generation;
    char tree_day[17];
    char error[256];
} Store;

/* Store operations returning int use 1 for success, 0 for failure; s->error
 * explains a failure. Close the store after a failed open to release resources.
 * Open/create and validate the append-only log and the saved view. A missing
 * view on an existing log is an error; replay is never a replacement for it.
 * A crash tail after the last saved view is appended to that view verbatim. */
int store_open(Store *s, const char *dir);
void store_close(Store *s);
int store_save_view(Store *s);
int store_append(Store *s, const char *kind, const char *text,
                 unsigned long *first, unsigned long *number);
/* Images are pi blocks {type:"image",data:base64,mimeType:"image/..."}.
 * Text and images share one durable record; only the first split has images. */
int store_append_images(Store *s, const char *kind, const char *text,
                        const Json *images, unsigned long *first,
                        unsigned long *number);
char *store_message(Store *s, unsigned long i);
char *store_images(Store *s, unsigned long i); /* allocated JSON array, NULL if none/error */
Node *store_node(Store *s, Ref r); /* NULL if nonexistent or unbuilt */
Node *store_slot(Store *s, Ref r); /* allocate slot */
int store_build(Store *s, Ref r, const char *text); /* append once, fsync */
char *store_zoom(Store *s, unsigned long id, unsigned long n,
                 unsigned long page);
const char *store_date(Store *s, unsigned long id);
int store_valid_ref(Store *s, Ref r);
unsigned long ref_start(Ref r);
unsigned long ref_span(Ref r);
void view_init(View *v);
void view_free(View *v);
void view_add(View *v, Ref r);
void view_copy(View *to, const View *from);
size_t view_measure(Store *s, View *v);
/* Render the saved view's prefix, opening a node which crosses end. */
char *view_render(Store *s, View *v, unsigned long end, int built_only);
/* NULL when the complete prefix would exceed limit; never return a cut view. */
char *view_render_limit(Store *s, View *v, unsigned long end, int built_only,
                       size_t limit);
/* Most-due built sibling pair. Age is measured from the pair's last message.
 * max_lines != 0 selects a line budget (used to verify rollback equivalence).
 * Otherwise enforce sawtooth hi/lo bytes, retaining an unfinished batch. */
int view_shrink(Store *s, View *v, size_t hi, size_t lo, size_t max_lines);

#endif
