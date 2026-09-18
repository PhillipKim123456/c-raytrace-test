#ifndef PARALLEL_H
#define PARALLEL_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "raytrace.h"

typedef enum { MODE_SERIAL,MODE_THREADS,MODE_PROCESSES } RenderMode;
typedef struct Pool Pool;
typedef struct {
    double render_ms,cpu_ms;
    uint64_t rays;
    uint32_t tiles,pixels;
    int workers;
    WorkerStats worker[RT_MAX_WORKERS];
} FrameStats;

Pool *pool_create(int capacity);
void pool_destroy(Pool *pool);
int pool_render(Pool *pool,RenderMode mode,int workers,const RenderSettings *settings,FrameStats *stats);
const uint32_t *pool_pixels(const Pool *pool);
const char *pool_error(const Pool *pool);
int pool_worker_main(const char *mapping,int index);
double clock_ms(void);
const char *mode_name(RenderMode mode);
#endif
