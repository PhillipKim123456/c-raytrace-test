# C / Ray Lab

An interactive, CPU-only ray tracing demo in C11 for **Windows + MinGW-w64 GCC**.
Compare serial rendering, threads, and child processes on the same scene, with
live FPS, frame times, CPU usage, ray throughput, and worker load.

The scene includes reflective spheres, soft shadows, a checker floor, and an
orbiting sphere. Win32/GDI presents CPU-generated pixels; no GPU rendering or
third-party libraries are used.

## Build and run

From PowerShell in the project root:

~~~powershell
.\scripts\build.ps1 -Test -Run
~~~

Or use the supplied Makefile:

~~~powershell
mingw32-make
mingw32-make test
mingw32-make run
~~~

Both methods produce **build/raytrace.exe**. The PowerShell script also works
when called by absolute path from another directory.

To compile directly:

~~~powershell
New-Item -ItemType Directory -Force build | Out-Null
gcc -std=c11 -O3 -Wall -Wextra -Wpedantic -Werror -Iinclude src/main.c src/parallel.c src/raytrace.c -o build/raytrace.exe -lgdi32 -luser32 -lm
.\build\raytrace.exe
~~~

The default is 640 x 360, one sample per pixel, threads, and up to eight workers.
Use `.\build\raytrace.exe --workers 16` to make up to 16 workers available.
The maximum is 32; more workers are not necessarily faster.

## Project layout

~~~text
c-raytrace-test/
|-- src/                  C implementations, UI, and built-in self-test
|-- include/              Shared C module headers
|-- scripts/
|   +-- build.ps1         PowerShell build, test, and run entry point
|-- docs/
|   |-- architecture.md   Rendering and synchronization design
|   +-- performance.md    Metric definitions and benchmark instructions
|-- build/                Generated executable and local results (ignored)
|-- .editorconfig         Editor formatting defaults
|-- .gitattributes        Git text and line-ending rules
|-- .gitignore            Build/output and local-file exclusions
|-- Makefile              GCC build, run, test, benchmark, and clean targets
|-- LICENSE               Existing project license
+-- README.md
~~~

Build output is kept out of the source tree. `mingw32-make clean` removes the
executable but preserves exported images and benchmark results. The next build
recreates it. The existing repository license applies.

## Controls

| Input | Action |
| --- | --- |
| `1`, `2`, `3` or mode buttons | Serial, threads, processes |
| `-` / `+` | Change active workers within the startup pool capacity |
| Drag in the scene / arrow keys | Orbit camera |
| Mouse wheel | Zoom |
| `R` | Cycle 480 x 270, 640 x 360, 960 x 540, 1280 x 720 |
| `Q` | Toggle one or four samples per pixel |
| `Space` | Freeze / resume animation; rendering continues |
| `Home` | Reset camera and animation phase |
| `Esc` | Close |

The window remains responsive during rendering. 
Changes take effect at frameboundaries. 
Worker pools start on first use and remain available until exit.
On native Windows, process mode uses CreateProcess and shared memory instead of POSIX fork.

## Verification and benchmarks

~~~powershell
.\build\raytrace.exe --self-test --workers 8
.\build\raytrace.exe --benchmark --workers 8 --frames 60
.\build\raytrace.exe --benchmark --frames 20 --output build\scene.ppm
~~~

The self-test compares full pixel buffers byte-for-byte and checks ray, pixel,
and tile counts across all modes. It covers one and multiple workers, tiny
images, partial edge tiles, both sample counts, scene changes, and pool reuse.
It exits nonzero on failure. Tests remain built into the executable so they
exercise the same renderer and child-process entry point as the demo.

The benchmark uses a fixed scene and reports CSV-formatted timing and throughput
rows after warm-up. `--help` lists all options.

See [performance and benchmarking](docs/performance.md) for exact metric
definitions and [rendering architecture](docs/architecture.md) for the design.
