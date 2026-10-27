# rtl_stream alpha: implementation and validation

This documents the implemented CPU path, not a performance claim. Sustainable
capture throughput, client capacity and hardware loss rates are unmeasured.
The server is experimental, unauthenticated, and intended for trusted networks.
Linux and macOS CPU builds are tested in CI; Windows and VkFFT are not validated.

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
GPU build/runtime behavior is outside the current CPU test coverage.

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

Root and standalone `tests/` CMake builds register eight native tests:
`test_dsp`, `test_dsp_spectral`, `test_stream`, `test_ring`, `test_server`,
`test_proto`, `test_client_c`, and `test_client_cpp`. The spectral white-box test
includes the DSP implementation and must not link another copy. C++11 is enabled
only when building tests (`BUILD_TESTING` defaults to OFF in the root build).
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
capture, RF calibration, sustained real-time performance or the GPU backend.
