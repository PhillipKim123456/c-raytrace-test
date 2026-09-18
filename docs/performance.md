# Performance and benchmarking

## Statistics

- **Render FPS:** completed frames per second over the last 120 frames. Includes
  dispatch, rendering, collection, copying the finished image, and coordinator
  overhead. This is not the monitor refresh rate.
- **Display FPS:** distinct completed frames painted by the UI over roughly half
  a second. UI painting is requested at approximately 60 Hz; Windows timer
  scheduling and rendering speed can lower this.
- **Frame time / p95:** mean completion interval and nearest-rank 95th percentile
  over the last 120 frames, in milliseconds. The graph shows those intervals.
- **Render + dispatch:** mean time to dispatch tiles, execute rendering, and
  wait for all active workers. Excludes pool startup and the copy for display.
- **CPU / whole machine:** user + kernel CPU time for the parent and its child
  processes divided by measured render wall time and logical CPU count. The
  core-equivalent figure omits the logical-CPU divisor. Includes UI work in the
  parent. Windows CPU accounting is coarse, so the rolling average is more useful
  than a single frame, and small overshoots are possible.
- **Ray throughput:** primary + reflection + shadow queries, including misses,
  per render second for the latest completed frame. Total rays and tiles per
  frame are also shown.
- **Worker load:** tiles claimed by each active worker, left to right starting at
  worker zero. Bars are relative to the largest tile count in that frame; these
  are work-distribution bars, not per-thread CPU percentages. Some workers may
  receive zero tiles for very small images.

Timing history resets when mode, worker count, resolution, or quality changes.
The first frame of a new configuration uses render time for its interval to
exclude pool startup. Camera movement and animation change the workload, so use
the fixed-scene benchmark for comparisons. All timings use QueryPerformanceCounter.

## Repeatable benchmark and image output

~~~powershell
.\build\raytrace.exe --benchmark --workers 8 --frames 60
.\build\raytrace.exe --benchmark --workers 4 --width 960 --height 540 --samples 4 --frames 20 --output build\scene.ppm
~~~

The benchmark freezes the camera and scene, warms up each mode once, and reports
CSV-formatted rows: workers, mean/median/p95/min render time, render FPS, speedup
against serial, total CPU percentage, ray throughput, and pixel hash. Every mode
uses identical rendering settings and must produce the same hash. Startup is
excluded, while per-frame synchronization is included. Results vary with power
settings, other applications, logical versus physical cores, and workload size.
Run several times and vary `--workers` to explore scaling.

`--output` writes the final benchmark frame as a binary PPM. It is optional and
only valid with `--benchmark`. `--help` lists all options. `--seconds N` closes an
interactive session after N seconds, useful for smoke tests.
