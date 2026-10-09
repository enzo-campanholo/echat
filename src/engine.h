#ifndef UNII_ENGINE_H
#define UNII_ENGINE_H

#include "store.h"

#define VIEW_HIGH 128000
#define VIEW_LOW 64000
#define COMPACT_HIGH 32000
#define COMPACT_LOW 16000
#define WORKERS 8

typedef struct Job { Ref ref; unsigned long retry; } Job;
typedef struct Queue { Job *v; size_t head, n, cap; } Queue;
typedef struct Engine {
    Store *store;
    Queue ready, leaves, failed, deferred;
    View compact;
    unsigned long seen, contiguous;
    unsigned int inflight;
    int deferred_ready;
} Engine;

int engine_open(Engine *e, Store *s);
void engine_close(Engine *e);
int engine_sync(Engine *e); /* add new leaves; O(new messages), never tree scan */
int engine_next(Engine *e, Job *j); /* 1: model job, 0: no work, -1: disk error */
int engine_done(Engine *e, Job j, const char *text);
void engine_fail(Engine *e, Job j);
void engine_retry(Engine *e); /* called once on a new incoming message */
char *engine_task(Engine *e, Job j);
char *engine_context(Engine *e, Job j); /* NULL if complete prefix exceeds 32 KB */
int engine_caught_up(Engine *e, unsigned long before);

#endif
