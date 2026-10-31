#ifndef UNII_ENGINE_H
#define UNII_ENGINE_H

#include "store.h"

typedef struct Job {
    Ref ref;
    unsigned long retry; /* leaf already counted in inflight */
    unsigned int fails;
    long due; /* monotonic seconds */
} Job;
typedef struct Queue { Job *v; size_t head, n, cap; } Queue;
typedef struct Engine {
    Store *store;
    Queue ready, leaves, failed, deferred;
    View compact;
    unsigned long seen, contiguous;
    unsigned int inflight;
    int deferred_ready;
    void (*built)(void *opaque, Ref r);
    void *opaque;
} Engine;

/* Open, sync, done and fallback return 0 on success, -1 on storage failure. */
int engine_open(Engine *e, Store *s);
void engine_close(Engine *e);
int engine_sync(Engine *e);
int engine_next(Engine *e, Job *j); /* 1: model job, 0: no work, -1: storage error */
int engine_done(Engine *e, Job j, const char *text);
/* Only for terminal refusal or answered-empty results, not provider errors. */
int engine_fallback(Engine *e, Job j);
void engine_fail(Engine *e, Job j, long now);
void engine_retry(Engine *e);
int engine_tick(Engine *e, long now); /* number of jobs requeued */
long engine_deadline(Engine *e, long now); /* seconds until retry, or -1 if none */
char *engine_task(Engine *e, Job j);
char *engine_context(Engine *e, Job j); /* NULL if over 32 KB, or a read failed */
int engine_caught_up(Engine *e, unsigned long before);

#endif
