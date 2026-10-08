#ifndef UNII_PI_H
#define UNII_PI_H

#include "base.h"
#include <sys/types.h>

typedef struct PiConfig {
    const char *command, *extension, *system;
    const char *provider, *model, *effort;
    const char *tools, *provider_tools;
    int compaction, cache_gate;
} PiConfig;

typedef struct PiCallbacks {
    void *opaque;
    int (*log)(void *opaque, const char *kind, const char *text);
    void (*display)(void *opaque, const char *text, int thinking);
    /* Return allocated text, or NULL on error. args is borrowed. */
    char *(*memory)(void *opaque, const char *name, const Json *args);
    /* Optional allocated JSON image array accompanying a memory page. */
    char *(*memory_images)(void *opaque, const char *name, const Json *args);
} PiCallbacks;

typedef struct Pi {
    pid_t pid;
    int fd, writefd, toolfd;
    int busy, done, failed, first_response, compaction;
    int prefix_waiting, fresh_waiting;
    unsigned long serial;
    char *prefix_key, *prefix_id, *fresh_id, *pending;
    Buf input, tools, out, toolout, result, error;
    size_t out_pos, toolout_pos;
} Pi;

int pi_start(Pi *p, const PiConfig *config);
/* A fresh prompt replaces the Pi session before dispatching view + task. */
int pi_prompt(Pi *p, const char *view, const char *task, int fresh);
int pi_prompt_images(Pi *p, const char *view, const char *task,
                     const char *images_json, int fresh);
int pi_steer(Pi *p, const char *message);
int pi_abort(Pi *p);
int pi_release_prefix(Pi *p);
/* Drain both read descriptors and flush pending commands; never block. */
int pi_process(Pi *p, const PiCallbacks *callbacks);
/* Poll writefd for POLLOUT while out.len > out_pos. */
int pi_flush(Pi *p);
void pi_stop(Pi *p);

#endif
