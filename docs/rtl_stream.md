# rtl_stream alpha: implementation and validation

This documents the implemented CPU and optional Vulkan paths, not a performance claim. Sustainable
capture throughput, client capacity and hardware loss rates are unmeasured.
The server is experimental, unauthenticated, and intended for trusted networks.
Linux and macOS CPU builds are tested in CI. Vulkan has local Apple M4/MoltenVK
synthetic correctness coverage described below, not GPU CI or Windows validation.

## Data flow and ownership

```
USB unsigned interleaved I/Q bytes
  -> mutex-protected copying ring (bounded retention, sequence + generation)
     -> private I/Q reader -> capture-center DC notch -> signed NCO
        -> multistage FIR filtering/decimation -> int16 -> TCP
     -> private FFT reader -> Blackman-Harris window -> FFT/averaging -> TCP
```

The default ring retains 512 slots of 262144 bytes. One mutex protects complete
slot writes, copies, retention and cursor state. Readers never borrow storage;
DSP and socket operations run outside the ring lock. Even oversized callbacks
are split into slots, empty callbacks are ignored, and odd callbacks mark a gap
without publishing mispaired samples. An overwritten or old-generation cursor
reports a gap without silently advancing. I/Q readers disconnect on loss.

Workers are joinable, and a slot cannot be reused before its worker is joined.
The reaper owns final socket close and DSP cleanup. Stop shuts down sockets,
wakes condition waits and the poll control pipe, and joins workers outside the
client lock. Requests and each complete frame send have absolute five-second
deadlines. Capture is cancelled and joined before closing the dongle; signal
handlers only set a signal-safe flag and write a nonblocking self-pipe.

The capture frequency/rate describe one immutable session. Hardware setup errors
are checked, and metadata uses device readback. A frequency change increments a
mutex-protected epoch; every old client terminates, with best-effort frequency
and end events. No client clears a shared notification for other clients. New
connections cannot start a new capture session without restarting the server.

## I/Q request contract

Let `Fi` be capture rate, `Fo` output rate, `BW` full two-sided channel bandwidth,
and `offset = requested_frequency - capture_frequency` (signed Hz).

- `0 < Fo <= Fi <= UINT32_MAX`, `Fi % Fo == 0`, and `Fi/Fo <= 4096`.
- Requested and capture frequencies fit uint32; negative offsets stay negative.
- `BW == 0` selects `floor(4*Fo/5)`.
- Filtered requests require `0 < BW <= floor(4*Fo/5)`.
- The passband must fit: `2*abs(offset) + BW <= Fi`.
- Raw exception: `Fo == Fi`, `BW == Fi`, and `offset == 0` bypasses FIR filtering.
- The bounded filter planner can reject otherwise valid rates/bandwidths if
  the required filter exceeds its tap budget; allocation can also fail.

All modes retain a nominal 10 Hz DC notch at the **capture center**, before NCO
mixing. Signals near that center are attenuated, including in raw mode. Input
bytes are centered at 127.5; output components are scaled by 256 and saturated
to int16. Raw mode therefore does not mean byte-for-byte dongle passthrough.
Anti-alias FIR filtering precedes each stage's downsampling; there is no
unfiltered CIC shortcut. Headers report the exact accepted output rate.

## FFT behavior

The default FFT uses 1024 bins, a 4096-complex-sample hop, and four-window power
averaging. It collects contiguous bins and skips the rest of each actual hop;
callback boundaries do not change sample timing. The FFT header rate is the
**capture sample rate**, not an update frequency. Payloads are floating-point
power values, not calibrated RF power.

Subscribers wait for a completed spectrum; an initial zero allocation is not
sent as data. A gap clears the hop, accumulator and published snapshot, advances
the FFT generation, and disconnects older subscribers. Fresh subscriptions wait
for a new completed average. CPU is the default backend. `ENABLE_VKFFT=ON`
selects VkFFT/Vulkan at compile time, with no runtime fallback if GPU setup fails.

Both backends accept power-of-two sizes 2..16384 and compute a forward complex
FP32 FFT with negative exponent, no normalization and no bin shift. Windowing
is external. `fft_execute` returns 0 or -1; input/output arrays must be identical
or disjoint (partial overlap is rejected). Failure leaves caller output unchanged.
A failed GPU execution poisons its plan. The server stops on failure before
adding that window to the power accumulator or publishing a spectrum.

### Optional Vulkan build and lifecycle

```sh
cmake -S . -B build-vkfft -DBUILD_TESTING=ON -DENABLE_VKFFT=ON \
  -DTEST_VKFFT=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$VULKAN_SDK"
cmake --build build-vkfft --parallel 4
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 \
  ctest --test-dir build-vkfft --output-on-failure --timeout 60 -V
```

GPU builds require CMake >=3.18, Vulkan headers/loader and a compute-capable
driver, a glslang CONFIG package (including its SPIRV-Tools dependencies), and
a C++17-capable toolchain/runtime. The wrapper remains C11; CMake uses the C++
linker only for GPU targets. VkFFT v1.3.4 is fetched at immutable commit
`066a17c17068c0f11c9298d848c2976c71fad1c1`, without configuring upstream's benchmark
project. CPU-only configuration neither fetches VkFFT nor requires Vulkan/glslang.
CPU production with `BUILD_TESTING=OFF` remains C-only.

`TEST_VKFFT=ON` explicitly adds GPU tests; missing dependencies or GPU setup fail,
not skip. Standalone `cmake -S tests -B build-tests-gpu -DTEST_VKFFT=ON
-DCMAKE_PREFIX_PATH="$VULKAN_SDK"` uses the same setup without requiring libusb.
For an existing pinned checkout, CMake's `FETCHCONTENT_SOURCE_DIR_VKFFT` can
avoid another fetch. Ordinary CPU CI does not enable these GPU tests.

The backend selects the first compatible compute device, supports Vulkan
portability enumeration/subset (MoltenVK), and requests no surface extensions.
Each plan owns its instance/device, buffers, persistent coherent staging map,
command buffer and fence. One checked submission records upload, FFT, download
with transfer/compute/host barriers, followed by fence completion before CPU
readback. Coherency removes flush/invalidate requirements, not synchronization.
Device storage uses the logical byte size and actual Vulkan memory requirements;
VkFFT owns any additional scratch. Calls on one plan must be serialized by its
owner; distinct plans may coexist. Compiler initialization is once per process,
plan compilation is serialized, and a per-thread C locale avoids global locale
mutation. glslang is deliberately not finalized by plan destruction, so destroying
one plan cannot invalidate another plan or another compiler user.

**Shutdown is not bounded against a hung GPU driver.** Wrapper submissions wait
indefinitely; a wait error poisons the plan and drains the device (or observes
device loss) before resource release. Timeout does not imply cancellation.
Separately, pinned VkFFT's internal LUT initialization helpers still use a
100-second wait and may release pending resources on upstream failure paths.
The wrapper cannot repair cleanup already performed inside `initializeVkFFT`;
this remains a dependency safety limitation, not hardened or bounded shutdown.
Precomputed LUT twiddles are explicitly selected: automatic device sin/cos on
the tested M4 exceeded the fixed component tolerance at N=16384. No upstream
patches or runtime CPU fallback are used.

## RTLSTREAM/2 wire format

Version 2 intentionally rejects version 1. All integers, int16 components and
IEEE-754 binary32 FFT payloads are big-endian; no host-native struct layout is
sent. Counts are bounded before allocation or payload processing.

| Frame | Magic | Fields after magic | Bytes before payload |
|---|---|---|---|
| Request | `RT2R` | frequency u64, rate u64, bandwidth u64, mode u8 | 29 total |
| I/Q | `RT2I` | frequency u64, rate u64, sequence u64, component count u32 | 32 |
| FFT | `RT2F` | frequency u64, capture rate u64, sequence u64, bins u32 | 32 |
| Event | `RT2E` | event type u32 | 8 |

I/Q mode is 0, FFT mode is 1, and requests must match the listener. FFT requests
require frequency, rate and bandwidth all zero. I/Q counts scalar components,
not pairs: the count is even and at most 262144. FFT bins range from 2 to 65536.
Frequency-change event type 1 adds a u64 frequency (16 bytes total); stream-end
type 2 has no payload (8 bytes total). TCP may split or coalesce frames.

Use source-built clients from `stream_client/`. The tracked Linux binaries
`stream_client/go/go`, `stream_client/go/realtest`, and
`stream_client/go/test_client/rtl_test_client` are legacy v1 artifacts retained
for now, not compatible v2 release outputs. Rebuild into a separate directory;
see `stream_client/README.md` for commands. No compatibility negotiation exists.

## Tests and CI

Root and standalone `tests/` CMake builds retain the eight native tests:
`test_dsp`, `test_dsp_spectral`, `test_stream`, `test_ring`, `test_server`,
`test_proto`, `test_client_c`, and `test_client_cpp`, plus `test_fft_backend`.
The spectral white-box test
includes the DSP implementation and must not link another copy. C++ is enabled
only for tests (C++11 clients) or the GPU compiler dependency (C++17-capable
toolchain); `BUILD_TESTING` defaults to OFF.
Each native test has a 60-second timeout; a hang fails.
Go race tests/vet and Node tests run separately, as shown in the root README.

Push/PR CI runs CPU builds on Linux GCC/Clang and macOS Clang. Linux sanitizer
jobs separately run ASan/UBSan and TSan. TSan uses Ubuntu 22.04/LLVM 14 and sets
`vm.mmap_rnd_bits=28` on the ephemeral runner for its fixed shadow mapping. A
runtime incompatibility or sanitizer report fails the job, rather than silently
skipping coverage. Sanitizer support on other platforms is not asserted.

The release-only workflow runs native, Go and Node tests before installation,
tarball packaging and upload; Docker publication depends on those jobs. PR
workflows have read-only repository permission and no publishing credentials.
Tests exercise software with synthetic signals and loopback peers, not dongle
capture, RF calibration or sustained real-time performance.

Local Vulkan validation used Apple M4, MoltenVK 1.4.1, Vulkan SDK 1.4.341.1,
glslang 16.2.0, CMake 4.3.2 and Apple Clang 21.0.0. All 13 root native tests
passed with Khronos validation and synchronization validation enabled, with no
validation errors or warnings. CPU standalone ASan/UBSan and TSan each passed
all nine tests. This is one driver/toolchain combination, not portability or
throughput certification.

GPU tests compare both real and imaginary components against a test-only CPU
reference (plus a double-precision DFT for small sizes): zero, impulse, complex
DC, positive/negative tones and seeded random input at 2, 16, 1024 and 16384.
The fixed unit-input tolerance is `2e-5*sqrt(N) + 3e-5*abs(reference)` per component;
small DFT checks use `2e-5 + 2e-5*abs(reference)`. Maximum observed GPU/CPU component
error was 0.0009765625; tolerances were not relaxed after testing. Tests cover
in-place/disjoint buffers, preserved input, invalid sizes/overlap, repeat use,
multiple live plans, destruction order and concurrent creation. Stream tests
also check Blackman-Harris 1024-bin/4096-hop/four-window power averages against
a DFT and verify fragmentation equivalence.

White-box seams inject wrapper buffer creation/allocation/bind/map and
command reset/begin/end/submit/wait errors, including a synthetic timeout while
real GPU work must be drained. They assert unchanged output, poisoned plans and
no pending-resource release. A server worker fault test rejects the fourth window
without changing the prior three-window accumulator or publishing an average,
and verifies session stop. These are not a complete Vulkan mock: actual device
loss, hung drivers, all instance/device/fence/pool failures and upstream internal
allocation/LUT failures remain untested. GPU driver and prebuilt glslang binaries
are not covered by the CPU sanitizer runs. GPU CTests fail on validation-error
or synchronization-hazard output as well as nonzero exit and 60-second timeout.
