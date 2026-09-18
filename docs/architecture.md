# Rendering architecture

**Serial:** the coordinator renders the whole frame itself. The UI thread still
exists, but there is only one rendering worker.

**Threads:** a persistent pool of C runtime threads (`_beginthreadex`) shares the
address space. Start/done events form a frame barrier. Each worker atomically
claims a 16 x 16 pixel tile using `InterlockedIncrement`.

**Processes:** a persistent pool runs this same executable in internal worker
mode. Windows `CreateProcess` is used because native Windows does not support
POSIX `fork`. A named `CreateFileMapping` region contains settings, the atomic
tile counter, per-worker statistics, and pixels. Named events publish frame
settings and signal completion. Workers open the mapping independently; shared
structures contain no pointers. Thread and process pools use separate events.

A frame's settings remain immutable until all active workers finish. A tile has
exactly one owner, so its pixels need no locks. Each worker accumulates statistics
locally and publishes once before signaling completion, avoiding a contended
per-ray counter. The parent copies a completed frame into a separate display
buffer protected by a critical section, so the UI never reads a half-rendered
image. Atomic operations and Windows event synchronization supply the required
memory ordering; `volatile` alone is not a synchronization mechanism.

The parent detects unexpected worker exit during a frame and reports an error.
On normal shutdown, it signals the stop event and joins its workers. Process
workers also monitor the parent process handle and exit if the parent disappears.
The shutdown path can terminate an owned process that fails to stop promptly.

This is task/data parallelism across tiles. It uses the same scalar ray tracing
kernel for every mode. Explicit SIMD and GPU rendering are outside this demo.
Small frames can lose to worker wake-up and synchronization overhead;
processes are not inherently faster than threads because they are separate
processes.


## Source layout

- `src/raytrace.c` and `include/raytrace.h`: scene, intersections, lighting, and reflections.
- `src/parallel.c` and `include/parallel.h`: Windows pools, shared memory, scheduling, and timing.
- `src/main.c`: window, controls, statistics, benchmark, and self-test.
