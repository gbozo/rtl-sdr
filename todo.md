# rtl_stream remaining work

Baseline: `5fc3676` includes the seven audited fixes, RTLSTREAM/2 clients,
tests and CI. These are remaining validation gaps and proposed enhancements,
not a list of unfixed audited defects. See [architecture and limits](docs/rtl_stream.md).
Use only on trusted/protected networks: listeners bind all interfaces without
TLS or authentication. Synthetic tests do not establish real-time capacity.

## P1 — Validation and release qualification

- [ ] Validate sustained real-dongle multi-client capture with FFT disabled and
  enabled; exercise shutdown, USB unplug, signals and hardware startup errors.
  Record device/configuration, losses and observed cleanup behavior; GPU driver
  waits are not guaranteed bounded.
- [ ] Add a USB/main stub harness that deterministically races cancellation
  against entry into `rtlsdr_read_async` and verifies capture join before device
  close, including startup-error paths.
- [ ] Inject client-worker `pthread_create` failure deterministically and verify
  cleanup and slot recovery. FFT thread-start failure, allocation failure and
  32-client saturation already have tests; do not treat them as missing coverage.
- [ ] Confirm actual Linux CI results and runtime behavior for this revision,
  including GCC/Clang, ASan/UBSan, TSan and client tests. CI already exists;
  recorded macOS CPU native 9/9 and sanitizer verification is not Linux evidence.
- [ ] Replace legacy tracked v1 Go binaries with rebuilt, qualified v2 release
  artifacts. Perform clean-install C/C++/Go/Node smoke tests against the v2 server
  for I/Q, FFT and shutdown, and qualify packaged release outputs.

## P2 — Suggested replay, observability and benchmark tooling

- [ ] Add raw unsigned interleaved I/Q replay through the same capture ingest
  path, with explicit capture frequency/rate metadata and paced versus
  maximum-throughput modes. Keep replay results distinct from dongle validation.
- [ ] Expose capture byte/sample/callback counts, ring overruns, reader lag and
  loss disconnects, DSP/FFT processing time and socket-blocking time.
- [ ] Build a reproducible 1/4/8/16-client benchmark matrix with FFT on/off and
  representative accepted bandwidths/output rates: exact integer capture-rate
  divisors within ratio, passband and filter-plan limits. Report machine, build,
  capture configuration, CPU, RSS, throughput, latency and drops; include a
  one-hour paced qualification run. Do not reuse historical CPU claims. Measure
  stronger FIR and private-copy costs before optimizing away safety guarantees.

## P3 — Vulkan milestone and remaining qualification

- [x] Complete the bounded VkFFT/Vulkan correctness milestone on Apple M4 with
  MoltenVK 1.4.1. Reviewed coder evidence records CPU 9/9 and GPU-enabled root and
  standalone suites each 13/13 passing, with Khronos validation and synchronization
  validation confirmed active, zero validation errors and no unexpected warnings.
  Coverage includes real/imaginary CPU parity, production window/hop/averaging,
  fragmentation, multiple live plans, wrapper fault injection and server stop
  before failed-window accumulation/publication. CPU ASan/UBSan and TSan each
  passed 9/9. This is synthetic correctness evidence, not sustained RF throughput
  or exhaustive failure-path qualification; see [details](docs/rtl_stream.md).
- [ ] Evaluate/fix upstream VkFFT v1.3.4 LUT initialization's 100-second timeout
  and internal cleanup hazard: upstream may release resources while GPU work is
  pending, before the wrapper can intervene. Revalidate any dependency change.
- [ ] Exercise actual device loss and driver hangs. Wrapper waits may remain
  unbounded while establishing completion or device loss before resource release;
  injected wait/timeout failures do not prove bounded shutdown on a hung driver.
- [ ] Validate GPU runtime portability on Linux and other Vulkan drivers/devices,
  including AMD, Intel and NVIDIA: repeat parity, lifecycle and active validation
  checks. Apple M4/MoltenVK results are not evidence for those platforms.
- [ ] Evaluate sanitizer coverage for GPU-side host libraries, including VkFFT
  integration and glslang. Existing CPU sanitizer runs do not instrument the GPU
  driver or prebuilt glslang libraries.
- [ ] Measure sustained CPU-versus-GPU throughput using comparable workloads and
  recorded hardware/build/configuration, including losses and latency. No speedup
  or real-time capacity is established by current tests. Keep the backend a
  compile-time alternative with no runtime CPU fallback.
