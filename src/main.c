#include "parallel.h"
#include <windowsx.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

#define HISTORY 120
#define SIDEBAR 280
#define BG RGB(13,18,26)
#define PANEL RGB(20,28,39)
#define FG RGB(226,234,243)
#define MUTED RGB(136,154,173)
#define ACCENT RGB(94,225,180)

typedef struct {
    Pool *pool;
    HANDLE renderer,stop;
    CRITICAL_SECTION lock;
    RenderSettings requested,presented;
    RenderMode mode,presented_mode;
    int workers,capacity,logical_cpus,animate,epoch,presented_epoch;
    uint32_t *front;
    FrameStats stats;
    double history[HISTORY],cpu_history[HISTORY],render_history[HISTORY];
    int history_count,history_next;
    uint64_t frame_id,displayed_id;
    double display_fps,display_start,start_time,auto_seconds;
    unsigned display_count;
    char error[256];
    HFONT title_font,body_font,small_font,number_font;
    int dragging,mouse_x,mouse_y;
} App;

static App app;

static double clamp(double x,double low,double high) { return x<low?low:x>high?high:x; }
static int compare_double(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;
    return (x>y)-(x<y);
}
static double percentile(double *values,int count,double p) {
    qsort(values,(size_t)count,sizeof(*values),compare_double);
    int index=(int)ceil((double)count*p)-1;
    return values[index<0?0:index];
}

static unsigned __stdcall render_main(void *unused) {
    (void)unused;
    int last_epoch=-1;
    double previous=clock_ms();
    for (;;) {
        if (WaitForSingleObject(app.stop,0)==WAIT_OBJECT_0) break;
        RenderSettings settings;
        RenderMode mode;
        int workers,epoch;
        EnterCriticalSection(&app.lock);
        settings=app.requested; mode=app.mode; workers=app.workers; epoch=app.epoch;
        LeaveCriticalSection(&app.lock);
        FrameStats stats;
        if (!pool_render(app.pool,mode,workers,&settings,&stats)) {
            EnterCriticalSection(&app.lock);
            snprintf(app.error,sizeof(app.error),"%s",pool_error(app.pool));
            LeaveCriticalSection(&app.lock);
            break;
        }
        EnterCriticalSection(&app.lock);
        memcpy(app.front,pool_pixels(app.pool),(size_t)settings.width*(size_t)settings.height*sizeof(uint32_t));
        double now=clock_ms();
        double elapsed=now-previous;
        if (epoch!=last_epoch) {
            app.history_count=0; app.history_next=0;
            /* Exclude one-time pool startup from steady-state UI statistics. */
            elapsed=stats.render_ms;
            last_epoch=epoch;
        }
        int i=app.history_next;
        app.history[i]=elapsed;
        app.cpu_history[i]=stats.cpu_ms;
        app.render_history[i]=stats.render_ms;
        app.history_next=(i+1)%HISTORY;
        if (app.history_count<HISTORY) ++app.history_count;
        app.presented=settings; app.presented_mode=mode; app.presented_epoch=epoch;
        app.stats=stats;
        ++app.frame_id;
        previous=now;
        LeaveCriticalSection(&app.lock);
    }
    return 0;
}

static void box(HDC dc,int x,int y,int w,int h,COLORREF color) {
    RECT r={x,y,x+w,y+h};
    HBRUSH brush=CreateSolidBrush(color);
    FillRect(dc,&r,brush);
    DeleteObject(brush);
}

static void label(HDC dc,int x,int y,const char *text,HFONT font,COLORREF color) {
    HGDIOBJ old=SelectObject(dc,font);
    SetTextColor(dc,color);
    SetBkMode(dc,TRANSPARENT);
    TextOutA(dc,x,y,text,(int)strlen(text));
    SelectObject(dc,old);
}

static void metric(HDC dc,int y,const char *name,const char *value) {
    label(dc,22,y,name,app.small_font,MUTED);
    label(dc,22,y+20,value,app.body_font,FG);
}

static void paint(HWND window,HDC target) {
    RECT client;
    GetClientRect(window,&client);
    int width=client.right,height=client.bottom;
    HDC dc=CreateCompatibleDC(target);
    HBITMAP bitmap=CreateCompatibleBitmap(target,width,height);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bitmap=SelectObject(dc,bitmap);
    box(dc,0,0,width,height,BG);
    box(dc,0,0,SIDEBAR,height,PANEL);
    label(dc,22,19,"C / RAY LAB",app.title_font,FG);
    label(dc,22,50,"CPU PARALLELISM DEMO",app.small_font,ACCENT);

    FrameStats stats;
    RenderSettings settings;
    RenderMode shown,requested;
    double history[HISTORY],cpu_total=0,render_total=0,frame_total=0;
    int count,next,workers,animate,has_frame,pending;
    char error[256];
    EnterCriticalSection(&app.lock);
    stats=app.stats; settings=app.presented; shown=app.presented_mode;
    requested=app.mode; workers=app.workers; animate=app.animate;
    count=app.history_count; next=app.history_next; has_frame=app.frame_id!=0;
    pending=app.epoch!=app.presented_epoch;
    memcpy(history,app.history,sizeof(history));
    snprintf(error,sizeof(error),"%s",app.error);
    for (int i=0;i<count;++i) {
        frame_total+=app.history[i]; cpu_total+=app.cpu_history[i]; render_total+=app.render_history[i];
    }
    int pane_x=SIDEBAR+24,pane_y=88,pane_w=width-pane_x-24,pane_h=height-300;
    if (has_frame) {
        double scale=fmin((double)pane_w/settings.width,(double)pane_h/settings.height);
        int draw_w=(int)(settings.width*scale),draw_h=(int)(settings.height*scale);
        int x=pane_x+(pane_w-draw_w)/2,y=pane_y+(pane_h-draw_h)/2;
        BITMAPINFO info={0};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=settings.width;
        info.bmiHeader.biHeight=-settings.height;
        info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        SetStretchBltMode(dc,COLORONCOLOR);
        StretchDIBits(dc,x,y,draw_w,draw_h,0,0,settings.width,settings.height,app.front,&info,DIB_RGB_COLORS,SRCCOPY);
        if (app.displayed_id!=app.frame_id) {
            ++app.display_count;
            app.displayed_id=app.frame_id;
        }
    }
    LeaveCriticalSection(&app.lock);
    double now=clock_ms();
    if (now-app.display_start>=500) {
        app.display_fps=(double)app.display_count*1000.0/(now-app.display_start);
        app.display_start=now; app.display_count=0;
    }

    char text[256];
    const char *descriptions[]={"One rendering worker","Shared address space","Shared memory + child workers"};
    for (int i=0;i<3;++i) {
        int y=94+i*57;
        int selected=(int)requested==i;
        box(dc,14,y,252,49,selected?RGB(33,54,57):BG);
        if (selected) box(dc,14,y,3,49,ACCENT);
        snprintf(text,sizeof(text),"%d  %s",i+1,mode_name((RenderMode)i));
        label(dc,26,y+6,text,app.body_font,selected?ACCENT:FG);
        label(dc,26,y+29,descriptions[i],app.small_font,MUTED);
    }
    label(dc,22,278,"RENDER FPS",app.small_font,MUTED);
    snprintf(text,sizeof(text),"%.1f",frame_total>0?1000.0*count/frame_total:0);
    label(dc,20,297,text,app.number_font,ACCENT);
    snprintf(text,sizeof(text),"Display: %.1f fps",app.display_fps);
    label(dc,24,351,text,app.small_font,MUTED);
    double sorted[HISTORY];
    memcpy(sorted,history,sizeof(sorted));
    double p95=count?percentile(sorted,count,0.95):0;
    snprintf(text,sizeof(text),"%.2f ms  /  %.2f ms",count?frame_total/count:0,p95);
    metric(dc,391,"FRAME TIME / P95",text);
    snprintf(text,sizeof(text),"%.2f ms",count?render_total/count:0);
    metric(dc,447,"RENDER + DISPATCH",text);
    double cores=render_total>0?cpu_total/render_total:0;
    snprintf(text,sizeof(text),"%.1f%%   (%.2f cores)",100.0*cores/app.logical_cpus,cores);
    metric(dc,503,"CPU / WHOLE MACHINE",text);
    snprintf(text,sizeof(text),"%.2f M rays/s",stats.render_ms>0?(double)stats.rays/stats.render_ms/1000.0:0);
    metric(dc,559,"RAY THROUGHPUT",text);
    snprintf(text,sizeof(text),"%d active / %d available",requested==MODE_SERIAL?1:workers,app.capacity);
    metric(dc,615,"WORKERS   [ - / + ]",text);
    snprintf(text,sizeof(text),"%.2f M rays / frame",(double)stats.rays/1000000.0);
    label(dc,22,683,text,app.small_font,MUTED);
    snprintf(text,sizeof(text),"%u tiles / frame  |  16 x 16 px",stats.tiles);
    label(dc,22,705,text,app.small_font,MUTED);

    label(dc,pane_x,20,"Reflections. Soft shadows. Shared work.",app.title_font,FG);
    if (error[0]) snprintf(text,sizeof(text),"ERROR: %.200s",error);
    else if (!has_frame) snprintf(text,sizeof(text),"Starting renderer...");
    else snprintf(text,sizeof(text),"%s%s  |  %d x %d  |  %d sample%s/pixel  |  %s",
        mode_name(shown),pending?" (switch pending)":"",settings.width,settings.height,settings.samples,
        settings.samples==1?"":"s",animate?"animation on":"animation frozen");
    label(dc,pane_x,55,text,app.small_font,error[0]?RGB(255,134,124):MUTED);

    int chart_y=height-186,chart_h=58;
    label(dc,pane_x,chart_y-24,"FRAME TIME  /  LAST 120 FRAMES",app.small_font,MUTED);
    double max_ms=16.67;
    for (int i=0;i<count;++i) if (history[i]>max_ms) max_ms=history[i];
    box(dc,pane_x,chart_y,pane_w,chart_h,PANEL);
    if (count>1) {
        HPEN pen=CreatePen(PS_SOLID,2,ACCENT);
        HGDIOBJ old_pen=SelectObject(dc,pen);
        for (int i=0;i<count;++i) {
            int index=count==HISTORY?(next+i)%HISTORY:i;
            int x=pane_x+(i*pane_w)/(HISTORY-1);
            int y=chart_y+chart_h-3-(int)((chart_h-6)*history[index]/max_ms);
            if (!i) MoveToEx(dc,x,y,NULL); else LineTo(dc,x,y);
        }
        SelectObject(dc,old_pen); DeleteObject(pen);
    }
    snprintf(text,sizeof(text),"%.1f ms",max_ms);
    label(dc,pane_x+pane_w-82,chart_y+3,text,app.small_font,MUTED);

    int bar_y=height-99;
    label(dc,pane_x,bar_y-23,"WORKER LOAD  /  TILES CLAIMED",app.small_font,MUTED);
    if (has_frame) {
        int gap=4,bw=(pane_w-(stats.workers-1)*gap)/stats.workers;
        uint32_t max_tiles=1;
        for (int i=0;i<stats.workers;++i) if (stats.worker[i].tiles>max_tiles) max_tiles=stats.worker[i].tiles;
        for (int i=0;i<stats.workers;++i) {
            int x=pane_x+i*(bw+gap);
            box(dc,x,bar_y,bw,7,PANEL);
            box(dc,x,bar_y,(int)((double)bw*stats.worker[i].tiles/max_tiles),7,ACCENT);
            snprintf(text,sizeof(text),"%u",stats.worker[i].tiles);
            label(dc,x,bar_y+12,text,app.small_font,MUTED);
        }
    }
    snprintf(text,sizeof(text),"Drag: orbit   Wheel: zoom   R: resolution   Q: quality   Space: animation   Home: reset");
    label(dc,pane_x,height-32,text,app.small_font,MUTED);
    BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY);
    SelectObject(dc,old_bitmap);
    DeleteObject(bitmap); DeleteDC(dc);
}

static void change_key(WPARAM key) {
    EnterCriticalSection(&app.lock);
    if (key>='1' && key<='3') { app.mode=(RenderMode)(key-'1'); ++app.epoch; }
    else if (key==VK_ADD || key==VK_OEM_PLUS) { if (app.workers<app.capacity) { ++app.workers; ++app.epoch; } }
    else if (key==VK_SUBTRACT || key==VK_OEM_MINUS) { if (app.workers>1) { --app.workers; ++app.epoch; } }
    else if (key=='Q') { app.requested.samples=app.requested.samples==1?4:1; ++app.epoch; }
    else if (key=='R') {
        const int widths[]={480,640,960,1280},heights[]={270,360,540,720};
        int next=0;
        for (int i=0;i<4;++i) if (app.requested.width==widths[i]) next=(i+1)%4;
        app.requested.width=widths[next]; app.requested.height=heights[next]; ++app.epoch;
    } else if (key==VK_SPACE) app.animate=!app.animate;
    else if (key==VK_HOME) {
        RenderSettings defaults=rt_default_settings();
        app.requested.yaw=defaults.yaw; app.requested.pitch=defaults.pitch;
        app.requested.distance=defaults.distance; app.requested.time=0; ++app.epoch;
    } else if (key==VK_LEFT) app.requested.yaw-=0.1f;
    else if (key==VK_RIGHT) app.requested.yaw+=0.1f;
    else if (key==VK_UP) app.requested.pitch=(float)clamp(app.requested.pitch+0.05,0.04,1.3);
    else if (key==VK_DOWN) app.requested.pitch=(float)clamp(app.requested.pitch-0.05,0.04,1.3);
    LeaveCriticalSection(&app.lock);
}

static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info=(MINMAXINFO *)lparam;
        info->ptMinTrackSize.x=1120; info->ptMinTrackSize.y=760;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(window,&ps);
        paint(window,dc);
        EndPaint(window,&ps);
        return 0;
    }
    case WM_TIMER: {
        static double last=0;
        double now=clock_ms(),delta=last?now-last:0;
        last=now;
        EnterCriticalSection(&app.lock);
        if (app.animate) app.requested.time+=(float)(delta*0.00055);
        LeaveCriticalSection(&app.lock);
        if (app.auto_seconds>0 && now-app.start_time>=app.auto_seconds*1000) PostMessageA(window,WM_CLOSE,0,0);
        if (!IsIconic(window)) InvalidateRect(window,NULL,FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam==VK_ESCAPE) PostMessageA(window,WM_CLOSE,0,0);
        else change_key(wparam);
        return 0;
    case WM_LBUTTONDOWN: {
        int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
        if (x>=14 && x<=266 && y>=94 && y<94+3*57) {
            int mode=(y-94)/57;
            if ((y-94)%57<49) change_key((WPARAM)('1'+mode));
        } else if (x>SIDEBAR) {
            app.dragging=1; app.mouse_x=x; app.mouse_y=y; SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (app.dragging) {
            int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
            EnterCriticalSection(&app.lock);
            app.requested.yaw-=(float)(x-app.mouse_x)*0.006f;
            app.requested.pitch=(float)clamp(app.requested.pitch+(y-app.mouse_y)*0.006,0.04,1.3);
            LeaveCriticalSection(&app.lock);
            app.mouse_x=x; app.mouse_y=y;
        }
        return 0;
    case WM_LBUTTONUP: app.dragging=0; ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: app.dragging=0; return 0;
    case WM_MOUSEWHEEL:
        EnterCriticalSection(&app.lock);
        app.requested.distance=(float)clamp(app.requested.distance-GET_WHEEL_DELTA_WPARAM(wparam)/240.0,4.0,18.0);
        LeaveCriticalSection(&app.lock);
        return 0;
    case WM_DESTROY:
        SetEvent(app.stop); KillTimer(window,1); PostQuitMessage(0); return 0;
    default: return DefWindowProcA(window,message,wparam,lparam);
    }
}

static HFONT font(int height,int weight,const char *name) {
    return CreateFontA(-height,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,name);
}

static int run_window(RenderSettings settings,RenderMode mode,int workers,int logical,double seconds) {
    memset(&app,0,sizeof(app));
    app.requested=settings; app.presented=settings; app.mode=mode;
    app.workers=workers; app.capacity=workers; app.logical_cpus=logical; app.animate=1;
    app.presented_epoch=-1; app.auto_seconds=seconds;
    InitializeCriticalSection(&app.lock);
    app.pool=pool_create(workers);
    app.front=calloc((size_t)RT_MAX_WIDTH*RT_MAX_HEIGHT,sizeof(uint32_t));
    app.stop=CreateEventA(NULL,TRUE,FALSE,NULL);
    if (!app.pool || !app.front || !app.stop) {
        fprintf(stderr,"Could not allocate renderer resources.\n");
        if (app.stop) CloseHandle(app.stop);
        free(app.front); pool_destroy(app.pool); DeleteCriticalSection(&app.lock); return 1;
    }
    SetProcessDPIAware();
    HINSTANCE instance=GetModuleHandleA(NULL);
    WNDCLASSA wc={0};
    wc.lpfnWndProc=window_proc; wc.hInstance=instance;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.lpszClassName="CRayLabWindow";
    if (!RegisterClassA(&wc)) {
        fprintf(stderr,"Could not register window class.\n");
        CloseHandle(app.stop); free(app.front); pool_destroy(app.pool); DeleteCriticalSection(&app.lock); return 1;
    }
    app.title_font=font(23,FW_SEMIBOLD,"Segoe UI");
    app.body_font=font(17,FW_MEDIUM,"Segoe UI");
    app.small_font=font(12,FW_NORMAL,"Segoe UI");
    app.number_font=font(52,FW_SEMIBOLD,"Consolas");
    HWND window=CreateWindowExA(0,wc.lpszClassName,"C / Ray Lab - CPU parallelism",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1360,860,NULL,NULL,instance,NULL);
    int result=0;
    if (!window) { fprintf(stderr,"Could not create window.\n"); result=1; }
    else {
        app.start_time=app.display_start=clock_ms();
        app.renderer=(HANDLE)_beginthreadex(NULL,0,render_main,NULL,0,NULL);
        if (!app.renderer) { fprintf(stderr,"Could not start renderer.\n"); DestroyWindow(window); result=1; }
        else {
            SetTimer(window,1,16,NULL);
            ShowWindow(window,SW_SHOWDEFAULT);
            MSG message;
            BOOL received;
            while ((received=GetMessageA(&message,NULL,0,0))>0) {
                TranslateMessage(&message); DispatchMessageA(&message);
            }
            if (received==-1) { SetEvent(app.stop); result=1; }
            WaitForSingleObject(app.renderer,INFINITE); CloseHandle(app.renderer);
            if (app.error[0]) { fprintf(stderr,"%s\n",app.error); result=1; }
        }
    }
    pool_destroy(app.pool); CloseHandle(app.stop); free(app.front);
    DeleteObject(app.title_font); DeleteObject(app.body_font);
    DeleteObject(app.small_font); DeleteObject(app.number_font);
    UnregisterClassA(wc.lpszClassName,instance); DeleteCriticalSection(&app.lock);
    return result;
}

static int write_ppm(const char *path,const uint32_t *pixels,const RenderSettings *s) {
    FILE *file=fopen(path,"wb");
    if (!file) { perror(path); return 0; }
    int ok=fprintf(file,"P6\n%d %d\n255\n",s->width,s->height)>0;
    for (int i=0;ok && i<s->width*s->height;++i) {
        unsigned char rgb[3]={(unsigned char)(pixels[i]>>16),(unsigned char)(pixels[i]>>8),(unsigned char)pixels[i]};
        ok=fwrite(rgb,1,3,file)==3;
    }
    if (fclose(file)!=0) ok=0;
    return ok;
}

static int self_test(int workers) {
    Pool *pool=pool_create(workers);
    uint32_t *reference=malloc((size_t)RT_MAX_WIDTH*RT_MAX_HEIGHT*sizeof(uint32_t));
    if (!pool || !reference) { pool_destroy(pool); free(reference); return 1; }
    int ok=1;
    const int widths[]={1,32,97,161,79,97},heights[]={1,32,61,93,35,61};
    for (int test=0;test<6 && ok;++test) {
        RenderSettings s=rt_default_settings();
        s.width=widths[test]; s.height=heights[test]; s.samples=test%2?4:1;
        s.time=(float)test*0.53f; s.yaw+=(float)test*0.17f;
        FrameStats baseline;
        if (!pool_render(pool,MODE_SERIAL,1,&s,&baseline)) { ok=0; break; }
        size_t bytes=(size_t)s.width*(size_t)s.height*sizeof(uint32_t);
        memcpy(reference,pool_pixels(pool),bytes);
        for (int mode=0;mode<3 && ok;++mode) for (int pass=0;pass<2 && ok;++pass) {
            FrameStats stats;
            int active=pass?workers:1;
            if (!pool_render(pool,(RenderMode)mode,active,&s,&stats)) { ok=0; break; }
            ok=memcmp(reference,pool_pixels(pool),bytes)==0 && stats.rays==baseline.rays &&
                stats.pixels==(uint32_t)(s.width*s.height) && stats.tiles==(uint32_t)rt_tile_count(&s);
            if (!ok) fprintf(stderr,"Mismatch: case %d, %s, %d workers.\n",test,mode_name((RenderMode)mode),active);
        }
        printf("%s case %d: %dx%d, %d spp, hash=%016llx\n",ok?"PASS":"FAIL",test+1,s.width,s.height,s.samples,
            (unsigned long long)rt_hash(reference,s.width*s.height));
    }
    if (!ok && pool_error(pool)[0]) fprintf(stderr,"%s\n",pool_error(pool));
    pool_destroy(pool); free(reference);
    if (ok) puts("All modes match byte-for-byte; ray, pixel and tile counts match. Pools reused across frames and worker counts.");
    return ok?0:1;
}

static int benchmark(RenderSettings settings,int workers,int frames,int logical,const char *output) {
    Pool *pool=pool_create(workers);
    double *times=malloc((size_t)frames*sizeof(double));
    if (!pool || !times) { pool_destroy(pool); free(times); return 1; }
    int ok=1;
    double baseline=0;
    uint64_t expected=0;
    printf("CPU ray tracing | %dx%d | %d spp | %d measured frames per mode | %d logical CPUs\n",
        settings.width,settings.height,settings.samples,frames,logical);
    puts("One warm-up frame per mode; startup excluded. Fixed camera and scene. FPS is render throughput.");
    puts("mode,workers,mean_ms,median_ms,p95_ms,min_ms,fps,speedup,cpu_percent,million_rays_per_s,pixel_hash");
    for (int mode=0;mode<3 && ok;++mode) {
        FrameStats stats;
        int active=mode==MODE_SERIAL?1:workers;
        if (!pool_render(pool,(RenderMode)mode,active,&settings,&stats)) { ok=0; break; }
        double sum=0,cpu=0;
        uint64_t rays=0;
        for (int frame=0;frame<frames;++frame) {
            if (!pool_render(pool,(RenderMode)mode,active,&settings,&stats)) { ok=0; break; }
            times[frame]=stats.render_ms; sum+=stats.render_ms; cpu+=stats.cpu_ms; rays+=stats.rays;
        }
        if (!ok) break;
        uint64_t hash=rt_hash(pool_pixels(pool),settings.width*settings.height);
        if (!mode) expected=hash;
        else if (hash!=expected) { fprintf(stderr,"Benchmark output mismatch.\n"); ok=0; break; }
        double mean=sum/frames,p95=percentile(times,frames,0.95);
        double median=frames%2?times[frames/2]:(times[frames/2-1]+times[frames/2])*0.5;
        if (!mode) baseline=mean;
        printf("%s,%d,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.1f,%.2f,%016llx\n",
            mode_name((RenderMode)mode),active,mean,median,p95,times[0],1000.0/mean,baseline/mean,
            100.0*cpu/sum/logical,(double)rays/sum/1000.0,(unsigned long long)hash);
        fflush(stdout);
    }
    if (!ok && pool_error(pool)[0]) fprintf(stderr,"%s\n",pool_error(pool));
    if (ok && output) ok=write_ppm(output,pool_pixels(pool),&settings);
    pool_destroy(pool); free(times);
    return ok?0:1;
}

static void usage(void) {
    puts("C / Ray Lab -- native Windows, CPU-only C ray tracing\n"
         "  raytrace.exe                         Open the interactive demo\n"
         "  raytrace.exe --self-test              Compare all execution modes\n"
         "  raytrace.exe --benchmark [options]    Benchmark all modes, fixed scene\n"
         "Options:\n"
         "  --workers N     Pool capacity / benchmark workers (1..32; default up to 8)\n"
         "  --width N       Render width (1..1280; default 640)\n"
         "  --height N      Render height (1..720; default 360)\n"
         "  --samples N     Samples per pixel (1 or 4; default 1)\n"
         "  --frames N      Measured benchmark frames (1..10000; default 20)\n"
         "  --mode NAME     Initial UI mode: serial, threads, processes\n"
         "  --output FILE   Save final benchmark image as binary PPM\n"
         "  --seconds N     Close the interactive demo after N seconds\n"
         "Controls: 1/2/3 mode; +/- workers; R resolution; Q quality; Space animation;\n"
         "          drag or arrows orbit; wheel zoom; Home reset camera; Esc quit.");
}

static int integer(const char *value,int min,int max,int *out) {
    char *end;
    errno=0;
    long n=strtol(value,&end,10);
    if (errno || !value[0] || *end || n<min || n>max) return 0;
    *out=(int)n;
    return 1;
}

int main(int argc,char **argv) {
    if (argc==4 && strcmp(argv[1],"--worker")==0) {
        int index;
        if (!integer(argv[3],0,RT_MAX_WORKERS-1,&index)) return 2;
        return pool_worker_main(argv[2],index);
    }
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    int logical=(int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (logical<1) logical=(int)system.dwNumberOfProcessors;
    if (logical<1) logical=1;
    int workers=logical<8?logical:8,frames=20,test=0,bench=0,seconds=0;
    RenderSettings settings=rt_default_settings();
    RenderMode mode=MODE_THREADS;
    const char *output=NULL;
    for (int i=1;i<argc;++i) {
        const char *arg=argv[i];
        if (!strcmp(arg,"--help") || !strcmp(arg,"-h")) { usage(); return 0; }
        if (!strcmp(arg,"--self-test")) { test=1; continue; }
        if (!strcmp(arg,"--benchmark")) { bench=1; continue; }
        if (i+1>=argc) { fprintf(stderr,"Missing value for %s\n",arg); return 2; }
        const char *value=argv[++i];
        int valid=1;
        if (!strcmp(arg,"--workers")) valid=integer(value,1,RT_MAX_WORKERS,&workers);
        else if (!strcmp(arg,"--width")) valid=integer(value,1,RT_MAX_WIDTH,&settings.width);
        else if (!strcmp(arg,"--height")) valid=integer(value,1,RT_MAX_HEIGHT,&settings.height);
        else if (!strcmp(arg,"--samples")) valid=integer(value,1,4,&settings.samples) && (settings.samples==1 || settings.samples==4);
        else if (!strcmp(arg,"--frames")) valid=integer(value,1,10000,&frames);
        else if (!strcmp(arg,"--seconds")) valid=integer(value,1,86400,&seconds);
        else if (!strcmp(arg,"--output")) output=value;
        else if (!strcmp(arg,"--mode")) {
            if (!strcmp(value,"serial")) mode=MODE_SERIAL;
            else if (!strcmp(value,"threads")) mode=MODE_THREADS;
            else if (!strcmp(value,"processes")) mode=MODE_PROCESSES;
            else valid=0;
        } else valid=0;
        if (!valid) { fprintf(stderr,"Invalid option/value: %s %s\n",arg,value); return 2; }
    }
    if (test && bench) { fprintf(stderr,"Choose --self-test or --benchmark.\n"); return 2; }
    if (output && !bench) { fprintf(stderr,"--output requires --benchmark.\n"); return 2; }
    if (test) return self_test(workers);
    if (bench) return benchmark(settings,workers,frames,logical,output);
    return run_window(settings,mode,workers,logical,(double)seconds);
}

