#include "parallel.h"
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RenderSettings settings;
    volatile LONG next_tile;
    DWORD parent_id;
    WorkerStats stats[RT_MAX_WORKERS];
    uint32_t pixels[RT_MAX_WIDTH*RT_MAX_HEIGHT];
} Shared;

typedef struct {
    Shared *shared;
    HANDLE stop,start,done;
    int index;
} ThreadContext;

typedef struct {
    HANDLE start[RT_MAX_WORKERS],done[RT_MAX_WORKERS],handle[RT_MAX_WORKERS];
    ThreadContext context[RT_MAX_WORKERS];
    int count;
} Backend;

struct Pool {
    int capacity;
    HANDLE mapping,stop;
    Shared *shared;
    Backend threads,processes;
    char name[128],error[256];
};

double clock_ms(void) {
    LARGE_INTEGER frequency,tick;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart*1000.0/(double)frequency.QuadPart;
}

const char *mode_name(RenderMode mode) {
    static const char *names[]={"Serial","Threads","Processes"};
    return names[(int)mode];
}

static void event_name(char *out,size_t size,const char *base,const char *kind,int index) {
    snprintf(out,size,"%s_%s_%d",base,kind,index);
}

static HANDLE make_event(const char *base,const char *kind,int index,BOOL manual) {
    char name[160];
    event_name(name,sizeof(name),base,kind,index);
    return CreateEventA(NULL,manual,FALSE,name);
}

static HANDLE open_event(const char *base,const char *kind,int index) {
    char name[160];
    event_name(name,sizeof(name),base,kind,index);
    return OpenEventA(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,name);
}

/* Each claimed tile owns disjoint pixels. Only the work counter is atomic.
   Collect locally to avoid false sharing; publish once before the done event. */
static void work(Shared *shared,int index) {
    Scene scene;
    WorkerStats stats={0};
    double start=clock_ms();
    rt_build_scene(&scene,&shared->settings);
    int count=rt_tile_count(&shared->settings);
    for (;;) {
        LONG tile=InterlockedIncrement(&shared->next_tile)-1;
        if (tile>=count) break;
        rt_render_tile(&scene,shared->pixels,(int)tile,&stats);
    }
    stats.busy_ms=clock_ms()-start;
    shared->stats[index]=stats;
}

static unsigned __stdcall thread_main(void *argument) {
    ThreadContext *c=argument;
    HANDLE waits[2]={c->stop,c->start};
    for (;;) {
        DWORD result=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
        if (result!=WAIT_OBJECT_0+1) break;
        work(c->shared,c->index);
        if (!SetEvent(c->done)) return 1;
    }
    return 0;
}

int pool_worker_main(const char *name,int index) {
    if (index<0 || index>=RT_MAX_WORKERS) return 2;
    HANDLE mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);
    if (!mapping) return 3;
    Shared *shared=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));
    if (!shared) { CloseHandle(mapping); return 3; }
    HANDLE stop=open_event(name,"stop",0);
    HANDLE start=open_event(name,"process_start",index);
    HANDLE done=open_event(name,"process_done",index);
    HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,shared->parent_id);
    int result=0;
    if (!stop || !start || !done || !parent) result=3;
    else {
        HANDLE waits[3]={stop,parent,start};
        for (;;) {
            DWORD wait=WaitForMultipleObjects(3,waits,FALSE,INFINITE);
            if (wait==WAIT_OBJECT_0 || wait==WAIT_OBJECT_0+1) break;
            if (wait!=WAIT_OBJECT_0+2) { result=4; break; }
            work(shared,index);
            if (!SetEvent(done)) { result=4; break; }
        }
    }
    if (parent) CloseHandle(parent);
    if (done) CloseHandle(done);
    if (start) CloseHandle(start);
    if (stop) CloseHandle(stop);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return result;
}

Pool *pool_create(int capacity) {
    if (capacity<1 || capacity>RT_MAX_WORKERS) return NULL;
    Pool *p=calloc(1,sizeof(*p));
    if (!p) return NULL;
    p->capacity=capacity;
    snprintf(p->name,sizeof(p->name),"Local\\CRay_%lu_%llu",(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    p->mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)sizeof(Shared),p->name);
    if (!p->mapping) { pool_destroy(p); return NULL; }
    p->shared=MapViewOfFile(p->mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));
    p->stop=make_event(p->name,"stop",0,TRUE);
    if (!p->shared || !p->stop) { pool_destroy(p); return NULL; }
    p->shared->parent_id=GetCurrentProcessId();
    return p;
}

static int backend_start(Pool *p,RenderMode mode) {
    Backend *b=mode==MODE_THREADS?&p->threads:&p->processes;
    if (b->count==p->capacity) return 1;
    for (int i=b->count;i<p->capacity;++i) {
        const char *start_kind=mode==MODE_THREADS?"thread_start":"process_start";
        const char *done_kind=mode==MODE_THREADS?"thread_done":"process_done";
        b->start[i]=make_event(p->name,start_kind,i,FALSE);
        b->done[i]=make_event(p->name,done_kind,i,TRUE);
        b->count=i+1; /* Include partially constructed resources in cleanup. */
        if (!b->start[i] || !b->done[i]) goto failed;
        if (mode==MODE_THREADS) {
            b->context[i]=(ThreadContext){p->shared,p->stop,b->start[i],b->done[i],i};
            b->handle[i]=(HANDLE)_beginthreadex(NULL,0,thread_main,&b->context[i],0,NULL);
            if (!b->handle[i]) goto failed;
        } else {
            char executable[32768],command[33024];
            DWORD length=GetModuleFileNameA(NULL,executable,(DWORD)sizeof(executable));
            if (!length || length>=sizeof(executable)) goto failed;
            snprintf(command,sizeof(command),"\"%s\" --worker \"%s\" %d",executable,p->name,i);
            STARTUPINFOA startup={0};
            PROCESS_INFORMATION info={0};
            startup.cb=sizeof(startup);
            if (!CreateProcessA(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&info)) goto failed;
            b->handle[i]=info.hProcess;
            CloseHandle(info.hThread);
        }
    }
    return 1;
failed:
    snprintf(p->error,sizeof(p->error),"Could not start %s worker (Windows error %lu).",mode_name(mode),(unsigned long)GetLastError());
    return 0;
}

static double process_cpu_ms(HANDLE process) {
    FILETIME created,exited,kernel,user;
    if (!GetProcessTimes(process,&created,&exited,&kernel,&user)) return 0;
    ULARGE_INTEGER k,u;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return ((double)k.QuadPart+(double)u.QuadPart)/10000.0;
}

static double total_cpu_ms(const Pool *p) {
    double sum=process_cpu_ms(GetCurrentProcess());
    for (int i=0;i<p->processes.count;++i)
        if (p->processes.handle[i]) sum+=process_cpu_ms(p->processes.handle[i]);
    return sum;
}

int pool_render(Pool *p,RenderMode mode,int workers,const RenderSettings *settings,FrameStats *stats) {
    if (p->error[0]) return 0;
    if (mode<MODE_SERIAL || mode>MODE_PROCESSES || workers<1 || workers>p->capacity ||
        settings->width<1 || settings->width>RT_MAX_WIDTH || settings->height<1 ||
        settings->height>RT_MAX_HEIGHT || (settings->samples!=1 && settings->samples!=4)) {
        snprintf(p->error,sizeof(p->error),"Invalid render configuration.");
        return 0;
    }
    if (mode!=MODE_SERIAL && !backend_start(p,mode)) return 0;
    memset(stats,0,sizeof(*stats));
    int active=mode==MODE_SERIAL?1:workers;
    p->shared->settings=*settings;
    memset(p->shared->stats,0,sizeof(p->shared->stats));
    InterlockedExchange(&p->shared->next_tile,0);
    double cpu_before=total_cpu_ms(p);
    double start=clock_ms();
    if (mode==MODE_SERIAL) work(p->shared,0);
    else {
        Backend *b=mode==MODE_THREADS?&p->threads:&p->processes;
        /* Reset all completions before publishing this immutable frame. */
        for (int i=0;i<active;++i) {
            if (!ResetEvent(b->done[i])) goto sync_failed;
        }
        for (int i=0;i<active;++i) {
            if (!SetEvent(b->start[i])) goto sync_failed;
        }
        for (;;) {
            DWORD wait=WaitForMultipleObjects((DWORD)active,b->done,TRUE,50);
            if (wait==WAIT_OBJECT_0) break;
            if (wait!=WAIT_TIMEOUT) goto sync_failed;
            for (int i=0;i<active;++i) {
                if (WaitForSingleObject(b->handle[i],0)!=WAIT_TIMEOUT) {
                    snprintf(p->error,sizeof(p->error),"%s worker %d exited during rendering.",mode_name(mode),i);
                    return 0;
                }
            }
        }
    }
    stats->render_ms=clock_ms()-start;
    stats->cpu_ms=total_cpu_ms(p)-cpu_before;
    stats->workers=active;
    for (int i=0;i<active;++i) {
        stats->worker[i]=p->shared->stats[i];
        stats->rays+=stats->worker[i].rays;
        stats->tiles+=stats->worker[i].tiles;
        stats->pixels+=stats->worker[i].pixels;
    }
    return 1;
sync_failed:
    snprintf(p->error,sizeof(p->error),"Worker synchronization failed (Windows error %lu).",(unsigned long)GetLastError());
    return 0;
}

static void backend_destroy(Backend *b,int processes) {
    for (int i=0;i<b->count;++i) {
        if (b->handle[i]) {
            if (processes && WaitForSingleObject(b->handle[i],5000)==WAIT_TIMEOUT)
                TerminateProcess(b->handle[i],1);
            WaitForSingleObject(b->handle[i],INFINITE);
            CloseHandle(b->handle[i]);
        }
        if (b->start[i]) CloseHandle(b->start[i]);
        if (b->done[i]) CloseHandle(b->done[i]);
    }
}

void pool_destroy(Pool *p) {
    if (!p) return;
    if (p->stop) SetEvent(p->stop);
    backend_destroy(&p->threads,0);
    backend_destroy(&p->processes,1);
    if (p->stop) CloseHandle(p->stop);
    if (p->shared) UnmapViewOfFile(p->shared);
    if (p->mapping) CloseHandle(p->mapping);
    free(p);
}

const uint32_t *pool_pixels(const Pool *p) { return p->shared->pixels; }
const char *pool_error(const Pool *p) { return p->error; }
