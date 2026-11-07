# Engine and presentation profiling

The Linux drivers in `tools/profiling/` need only ZxKit, its bundled ROMs and the normal
SafeCpp dependency. A consumer checkout is not needed. Generated workloads,
traces, commands and results belong under `build/profiling/`.

```sh
cmake -S . -B build/profiling/native -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZXKIT_BUILD_TESTS=ON
cmake --build build/profiling/native --parallel
c++ -std=c++23 -O2 -g -Iinclude -Ibuild/profiling/native/_deps/safecpp-src \
    tools/profiling/profile_engine.cpp build/profiling/native/libzxkit_video_filters.a \
    build/profiling/native/libzxkit.a -lz -pthread -o build/profiling/profile-engine
cc -shared -fPIC -O2 tools/profiling/profile_allocations.c -o build/profiling/allocations.so
python3 tools/profiling/make_profile_workload.py
build/profiling/profile-engine engine 15 tests/fixtures/roms build/profiling/workload.z80
build/profiling/profile-engine engine-rewind 15 tests/fixtures/roms build/profiling/workload.z80
build/profiling/profile-engine paused 15 tests/fixtures/roms build/profiling/workload.z80
build/profiling/profile-engine frames 5 tests/fixtures/roms build/profiling/workload.z80
build/profiling/profile-engine audio 5 tests/fixtures/roms -
build/profiling/profile-engine filters 5 tests/fixtures/roms - hq 4
perf record -F 499 --call-graph dwarf,8192 -o build/profiling/engine.data -- \
    build/profiling/profile-engine engine-rewind 15 tests/fixtures/roms build/profiling/workload.z80
perf report --stdio --no-children -i build/profiling/engine.data
perf stat -e task-clock,context-switches,cpu-migrations,page-faults -- \
    build/profiling/profile-engine engine 15 tests/fixtures/roms build/profiling/workload.z80
PROFILE_WARMUP=2 LD_PRELOAD="$PWD/build/profiling/allocations.so" \
    build/profiling/profile-engine engine-rewind 15 tests/fixtures/roms build/profiling/workload.z80
```

The direct compiler command assumes the default static library build and fetched
SafeCpp; use the installed SafeCpp include path when using an installed dependency.
`engine` uses real wall-clock pacing, callbacks and scanline locks. `engine-rewind`
also retains 300 snapshots, as a typical rewind-enabled host does. `paused` sleeps;
`frames`, `audio` and `filters` run without pacing and report throughput and CPU per
frame. The audio microbenchmark exercises a sounding AY channel at 48 kHz; it does
not represent silence. Filters use a deterministic 320×256 pattern and reusable
pipeline buffers, independent of GPU uploads. Enumerate IDs/scales from
`videoFilterCatalog()`; chained presets belong to the host.

The generated 48K snapshot has patterned video RAM, changing attributes, border
writes and beeper output. It is an authored stress workload, not a claim about
any particular game's instruction mix. Supply other media explicitly to check
representative games. Preserve its checksum and use identical media for comparisons.

## Measurement conventions

Process CPU percent of one logical core is `100 × CPU seconds / wall seconds`,
summing all process threads. Whole-machine percent divides that number by the
number of online logical CPUs (16 on the measured host). Throughput and time per
frame are more useful for the unpaced microbenchmarks. Keep the same compiler,
optimization, machine, speed, audio model and display workload across versions.
Use a detached worktree for the historical library; `ZXKIT_PROFILE_INITIAL`
lets the driver build against the initial library without its later filter API.

Production CPU runs have no profiler or allocation interposer attached. Collect
native stacks and allocation counts in separate runs. The glibc interposer counts
allocation requests and requested bytes after `PROFILE_WARMUP` seconds, including
reallocations; these are allocation traffic, not live heap or leaks. It does not
account for direct mmap, GPU or audio-server allocations. RSS/PSS, thread activity,
scene graph, texture uploads and QML memory must be measured by the consumer.
Voluntary context switches are a scheduling proxy, not an exact wakeup count.

## November 2026 investigation

The initial core (`e12c5fe`) and current core were built with GCC 16.2, `-O2 -g`,
on a Ryzen 7 5800H, with the same 48K workload and 100% speed. Native captures found
incremental ULA rendering to be a leading cost. The raster loop now resolves rows
and scanline divisions once per span, retaining every fetch clock and the exact
stop point before memory or border writes. `test_borders` compares large spans
against single-clock calls with intervening RAM/border changes, including odd and
zero borders and 48K/128K timings. The complete hardware suites remain mandatory.

In the initial five-second throughput runs, current unfiltered frame cost fell
from 0.3180 to 0.2754 CPU ms/frame (about 13%). These unpaced runs isolate the core;
consumer frame presentation can dominate the total. Paced Engine measurements
were 0.1399 CPU seconds before and 0.1260 after over five wall seconds, while the
rewind-enabled case remained within run variation. Both retained about 50 wakes
per second from frame pacing. A paused Engine used under 0.3 ms CPU over five
seconds, with no periodic polling. The sounding AY microbenchmark costs about
0.048 ms/frame; the historical 0.026 ms predates later sound-accuracy work.
Three closely paired runs measured 0.3304 ms/frame before and 0.2839 after
(14.1%). Every filter's CPU cost and the raw repeat series are in the local report.

The bilinear filter was a separate measured bottleneck: 4x took about 14.68
CPU ms/frame. Specializing its six integer scales and caching horizontal sample
coordinates in reusable scratch reduced it to 10.11 ms/frame (31%, three closely
paired runs). No interpolation, rounding or pixel format changed. Frozen original
kernel fingerprints cover every scale, edge clipping, RGB/alpha and asymmetric
sizes; composition tests also cover shared scratch across bilinear/HQ stages.
Detailed measurements, filter matrix, allocation logs and native traces from the
investigation are retained locally in `build/profiling/`. Presentation profiling
and application history are documented in the consumer's own repository.

Historical increases include accuracy work: incremental raster fetches, continuous
sample timing and band-limited AY synthesis. Reverting those to match a former CPU
percentage would change emulation or audio output. The independent host audio
producer/consumer architecture remains intact. Package energy counters and kernel
scheduler wakeup tracepoints were permission denied on this host; no battery-life
or process-energy claim is made from CPU percentages.
