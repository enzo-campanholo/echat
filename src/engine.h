#ifndef UNII_ENGINE_H
#define UNII_ENGINE_H

#include "store.h"

/* retry: a leaf already counted in inflight. fails: consecutive failures;
 * due: monotonic second of the next automatic retry. */
typedef struct Job { Ref ref; unsigned long retry; unsigned int fails; long due; } Job;
typedef struct Queue { Job *v; size_t head, n, cap; } Queue;
typedef struct Engine {
    Store *store;
    Queue ready, leaves, failed, deferred;
    View compact;
    unsigned long seen, contiguous;
    unsigned int inflight;
    int deferred_ready;
    void (*built)(void *opaque, Ref r); /* optional: each node as it is logged */
    void *opaque;
} Engine;

/* Functions returning int return 0 on success, -1 on a storage failure
 * (the store's error explains it), unless noted. */
int engine_open(Engine *e, Store *s);
void engine_close(Engine *e);
int engine_sync(Engine *e); /* add new leaves; O(new messages), never tree scan */
int engine_next(Engine *e, Job *j); /* 1: model job, 0: no work, -1: storage error */
int engine_done(Engine *e, Job j, const char *text);
void engine_fail(Engine *e, Job j, long now);
void engine_retry(Engine *e); /* a new message or an explicit retry: all failed */
int engine_tick(Engine *e, long now); /* due automatic retries; returns how many */
long engine_deadline(Engine *e, long now); /* seconds to the next one, or -1 */
char *engine_task(Engine *e, Job j);
char *engine_context(Engine *e, Job j); /* NULL if over 32 KB, or a read failed */
int engine_caught_up(Engine *e, unsigned long before);

#endif
