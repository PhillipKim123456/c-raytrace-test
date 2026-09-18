#include "raytrace.h"
#include <math.h>
#include <string.h>

static Vec3 v(float x,float y,float z) { return (Vec3){x,y,z}; }
static Vec3 add(Vec3 a,Vec3 b) { return v(a.x+b.x,a.y+b.y,a.z+b.z); }
static Vec3 sub(Vec3 a,Vec3 b) { return v(a.x-b.x,a.y-b.y,a.z-b.z); }
static Vec3 mul(Vec3 a,float b) { return v(a.x*b,a.y*b,a.z*b); }
static Vec3 had(Vec3 a,Vec3 b) { return v(a.x*b.x,a.y*b.y,a.z*b.z); }
static float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3 cross(Vec3 a,Vec3 b) { return v(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
static Vec3 unit(Vec3 a) { return mul(a,1.0f/sqrtf(dot(a,a))); }
static Vec3 mix(Vec3 a,Vec3 b,float t) { return add(mul(a,1-t),mul(b,t)); }

RenderSettings rt_default_settings(void) {
    return (RenderSettings){640,360,1,0.24f,0.24f,9.5f,0.0f};
}

void rt_build_scene(Scene *s,const RenderSettings *settings) {
    memset(s,0,sizeof(*s));
    s->settings=*settings;
    Vec3 target=v(0,0.85f,0);
    float r=settings->distance,cp=cosf(settings->pitch);
    s->eye=add(target,v(r*sinf(settings->yaw)*cp,r*sinf(settings->pitch),r*cosf(settings->yaw)*cp));
    s->forward=unit(sub(target,s->eye));
    s->right=unit(cross(s->forward,v(0,1,0)));
    s->up=cross(s->right,s->forward);
    s->sphere_count=7;
    s->spheres[0]=(Sphere){v(0,1.16f,0),v(0.83f,0.90f,0.97f),1.16f,0.88f};
    s->spheres[1]=(Sphere){v(-2.25f,0.82f,0.1f),v(0.82f,0.18f,0.085f),0.82f,0.30f};
    s->spheres[2]=(Sphere){v(2.08f,0.72f,-0.35f),v(0.035f,0.60f,0.49f),0.72f,0.22f};
    s->spheres[3]=(Sphere){v(0.9f,0.43f,2.05f),v(0.93f,0.57f,0.10f),0.43f,0.55f};
    s->spheres[4]=(Sphere){v(-1.0f,0.34f,1.95f),v(0.36f,0.14f,0.75f),0.34f,0.35f};
    s->spheres[5]=(Sphere){v(-1.8f,0.50f,-2.1f),v(0.12f,0.26f,0.65f),0.50f,0.42f};
    s->spheres[6]=(Sphere){v(2.55f*cosf(settings->time),1.5f+0.35f*sinf(settings->time*1.7f),2.55f*sinf(settings->time)),v(0.85f,0.45f,0.28f),0.25f,0.65f};
}

typedef struct { float t,reflection; Vec3 point,normal,color; } Hit;

/* Count primary, reflection and shadow queries, including misses. */
static int intersect(const Scene *s,Vec3 origin,Vec3 direction,float limit,Hit *hit,WorkerStats *stats) {
    ++stats->rays;
    float nearest=limit;
    int object=-2;
    for (int i=0;i<s->sphere_count;++i) {
        Vec3 oc=sub(origin,s->spheres[i].center);
        float b=dot(oc,direction),c=dot(oc,oc)-s->spheres[i].radius*s->spheres[i].radius;
        float discriminant=b*b-c;
        if (discriminant<0) continue;
        float root=sqrtf(discriminant),t=-b-root;
        if (t<0.001f) t=-b+root;
        if (t>0.001f && t<nearest) { nearest=t; object=i; }
    }
    if (fabsf(direction.y)>0.00001f) {
        float t=-origin.y/direction.y;
        if (t>0.001f && t<nearest) { nearest=t; object=-1; }
    }
    if (object==-2) return 0;
    if (!hit) return 1;
    hit->t=nearest;
    hit->point=add(origin,mul(direction,nearest));
    if (object>=0) {
        Sphere sphere=s->spheres[object];
        hit->normal=mul(sub(hit->point,sphere.center),1.0f/sphere.radius);
        hit->color=sphere.color;
        hit->reflection=sphere.reflection;
    } else {
        hit->normal=v(0,1,0);
        float parity=fmodf(floorf(hit->point.x)+floorf(hit->point.z),2.0f);
        hit->color=parity!=0?v(0.15f,0.19f,0.24f):v(0.30f,0.36f,0.41f);
        hit->reflection=0.18f;
    }
    return 1;
}

static Vec3 sky(Vec3 d) {
    float t=fmaxf(0.0f,d.y);
    Vec3 color=mix(v(0.43f,0.53f,0.67f),v(0.07f,0.13f,0.24f),sqrtf(t));
    /* Bright studio panel, visible in curved metal reflections. */
    if (d.y>0.25f && d.y<0.7f && d.x>-0.75f && d.x<-0.35f && d.z>0.1f)
        color=add(color,v(2.0f,1.8f,1.5f));
    return color;
}

static Vec3 trace(const Scene *s,Vec3 o,Vec3 d,int depth,WorkerStats *stats) {
    Hit h;
    if (!intersect(s,o,d,1000.0f,&h,stats)) return sky(d);
    Vec3 origin=add(h.point,mul(h.normal,0.003f));
    Vec3 color=mul(h.color,0.11f+0.11f*fmaxf(0,h.normal.y));
    const Vec3 lights[2]={{-3.5f,6.0f,3.0f},{4.0f,4.5f,-2.5f}};
    const Vec3 energy[2]={{1.9f,1.65f,1.4f},{0.55f,0.85f,1.3f}};
    const float offsets[4][2]={{-0.35f,-0.35f},{0.35f,-0.35f},{-0.35f,0.35f},{0.35f,0.35f}};
    for (int l=0;l<2;++l) for (int j=0;j<4;++j) {
        Vec3 to_light=sub(add(lights[l],v(offsets[j][0],0,offsets[j][1])),origin);
        float distance=sqrtf(dot(to_light,to_light));
        Vec3 ld=mul(to_light,1.0f/distance);
        float diffuse=fmaxf(0,dot(h.normal,ld));
        if (diffuse<=0 || intersect(s,origin,ld,distance,NULL,stats)) continue;
        Vec3 half_vector=unit(sub(ld,d));
        float specular=powf(fmaxf(0,dot(h.normal,half_vector)),80.0f)*0.48f;
        color=add(color,mul(had(add(mul(h.color,diffuse),v(specular,specular,specular)),energy[l]),0.25f));
    }
    if (depth<3) {
        Vec3 reflected=sub(d,mul(h.normal,2.0f*dot(d,h.normal)));
        float facing=1.0f-fmaxf(0,-dot(d,h.normal));
        float reflection=h.reflection+(1.0f-h.reflection)*powf(facing,5.0f);
        color=mix(color,trace(s,origin,reflected,depth+1,stats),reflection);
    }
    return mix(color,sky(d),1.0f-expf(-h.t*0.008f));
}

static unsigned char channel(float x) {
    x=fmaxf(0,x);
    return (unsigned char)(255.0f*sqrtf(x/(1.0f+x))+0.5f);
}

int rt_tile_count(const RenderSettings *s) {
    return ((s->width+RT_TILE_SIZE-1)/RT_TILE_SIZE)*((s->height+RT_TILE_SIZE-1)/RT_TILE_SIZE);
}

void rt_render_tile(const Scene *s,uint32_t *pixels,int tile,WorkerStats *stats) {
    const RenderSettings *p=&s->settings;
    int columns=(p->width+RT_TILE_SIZE-1)/RT_TILE_SIZE;
    int x0=(tile%columns)*RT_TILE_SIZE,y0=(tile/columns)*RT_TILE_SIZE;
    int x1=x0+RT_TILE_SIZE,y1=y0+RT_TILE_SIZE;
    if (x1>p->width) x1=p->width;
    if (y1>p->height) y1=p->height;
    const float samples[4][2]={{0.25f,0.25f},{0.75f,0.25f},{0.25f,0.75f},{0.75f,0.75f}};
    float aspect=(float)p->width/(float)p->height;
    for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
        Vec3 sum=v(0,0,0);
        for (int k=0;k<p->samples;++k) {
            float dx=p->samples==1?0.5f:samples[k][0],dy=p->samples==1?0.5f:samples[k][1];
            float u=(2.0f*((float)x+dx)/(float)p->width-1.0f)*aspect*0.46f;
            float w=(1.0f-2.0f*((float)y+dy)/(float)p->height)*0.46f;
            Vec3 direction=unit(add(s->forward,add(mul(s->right,u),mul(s->up,w))));
            sum=add(sum,trace(s,s->eye,direction,0,stats));
        }
        sum=mul(sum,1.0f/(float)p->samples);
        pixels[y*p->width+x]=((uint32_t)channel(sum.x)<<16)|((uint32_t)channel(sum.y)<<8)|channel(sum.z);
    }
    ++stats->tiles;
    stats->pixels+=(uint32_t)((x1-x0)*(y1-y0));
}

uint64_t rt_hash(const uint32_t *pixels,int count) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for (int i=0;i<count;++i) { hash^=pixels[i]; hash*=UINT64_C(1099511628211); }
    return hash;
}
