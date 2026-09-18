#ifndef RAYTRACE_H
#define RAYTRACE_H
#include <stdint.h>
#define RT_MAX_WIDTH 1280
#define RT_MAX_HEIGHT 720
#define RT_MAX_WORKERS 32
#define RT_TILE_SIZE 16
typedef struct { float x,y,z; } Vec3;
/* No pointers: settings and statistics also cross process boundaries. */
typedef struct { int width,height,samples; float yaw,pitch,distance,time; } RenderSettings;
typedef struct { uint64_t rays; uint32_t tiles,pixels; double busy_ms; unsigned char padding[40]; } WorkerStats;
typedef struct { Vec3 center,color; float radius,reflection; } Sphere;
typedef struct { RenderSettings settings; Vec3 eye,forward,right,up; Sphere spheres[7]; int sphere_count; } Scene;
RenderSettings rt_default_settings(void);
void rt_build_scene(Scene *scene,const RenderSettings *settings);
void rt_render_tile(const Scene *scene,uint32_t *pixels,int tile,WorkerStats *stats);
int rt_tile_count(const RenderSettings *settings);
uint64_t rt_hash(const uint32_t *pixels,int count);
#endif
