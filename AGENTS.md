# rtl-sdr fork: implementation guide

This fork includes multi-client `rtl_tcp`, device enumeration/`rtl_list`, and
the experimental `rtl_stream` channel server. See `README` and
`docs/rtl_stream.md` for the implemented architecture and limitations. Do not
describe alpha code or synthetic tests as measured real-time throughput.

## Device listing

`rtlsdr_device_enumerate` in `include/rtl-sdr.h` and `src/librtlsdr.c` enumerates
supported USB devices in one pass, invokes the supplied callback, and returns
the number found. A nonzero callback result stops enumeration. `rtl_list`
supports text output, `-j` JSON and `-j -p` pretty JSON.

## rtl_stream contracts

- `src/rtl_stream.c`: CLI, checked hardware configuration/readback, capture,
  signal-safe self-pipe, cancellation/join before device close.
- `src/rtl_ring.c/h`: bounded **mutex-protected copying ring**, not lock-free.
  Cursors carry sequence and generation. Only DATA advances a read cursor;
  loss reports GAP. Never reintroduce borrowed pointers to overwriteable slots.
- `src/rtl_stream_server.c/h`: poll listeners/control pipe, private reader/wire
  buffers, joinable workers, final cleanup by reaper, five-second request/frame
  deadlines. Never join under the client lock or do DSP/I/O under the ring lock.
- Capture metadata is immutable per session. Retuning changes an epoch visible
  to every client and terminates the session; events are best effort. I/Q gaps
  disconnect the reader. FFT gaps reset hop/accumulator/snapshot and disconnect
  old-generation subscribers. Empty/odd/oversized USB callbacks must not create
  silently truncated or mispaired streams.
- `src/rtl_dsp.c/h`: unsigned byte input centered at 127.5, nominal 10 Hz
  capture-center DC notch **before signed NCO mixing**, then multistage FIR
  filtering/decimation and int16 scaling/saturation. Filter before downsampling.
  Validate exact integer rate ratios 1..4096 and uint32 capture rates. Default
  BW is floor(4*output_rate/5); filtered BW cannot exceed that. Raw exception is
  equal input/output rates, full BW and zero offset, and still retains the notch.
  Require the full passband to fit within capture and honor bounded filter plans.
- `src/rtl_stream_proto.c/h`: intentionally incompatible **RTLSTREAM/2** with
  RT2R/RT2I/RT2F/RT2E magic and entirely big-endian headers/payloads. I/Q counts
  components (even, <=262144), FFT counts bins (2..65536). Listener mode must
  match; FFT request frequency/rate/BW are zero. FFT headers carry capture rate.
- `src/fft/`: CPU default, Blackman-Harris window, actual complex-sample hop
  continuity. VkFFT/Vulkan is a compile-time alternative, **not** runtime CPU
  fallback. GPU validation and throughput benchmarking remain outstanding.
- Client sources live under `stream_client/`; rebuild them for v2. Tracked old
  Linux Go executables remain v1 artifacts, not validated release binaries.

CLI: `rtl_stream -f 100.5M -s 2.4M -p 1234 -P 1235 -i 0`.
`-P 0` (default) disables FFT, `--list` lists devices/configuration. One process
selects one device; `-i` does not imply concurrent multi-dongle capture. Listeners
bind all interfaces without TLS/authentication: use only on a protected network.

## Build and verification

```
cmake -S . -B build -DBUILD_TESTING=ON -DENABLE_VKFFT=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --timeout 60
(cd stream_client/go && go test -race -timeout 60s ./... && go vet ./...)
node stream_client/node/client.test.js
```

Hardware-free native tests can also use `cmake -S tests -B build-tests` without
libusb. Production-only builds use `-DBUILD_TESTING=OFF` and do not require the
C++ client-test compiler. Tests use C11/C++11; `test_dsp_spectral.c` includes
`rtl_dsp.c` itself, so never link that implementation a second time. Preserve
test timeouts and fail hangs. Do not hide sanitizer failures with ignored exits.

`.github/workflows/test.yml` covers push/PR Linux GCC/Clang and macOS Clang CPU
builds plus Linux ASan/UBSan and TSan. `.github/workflows/build.yml` gates release
packaging/publication on native/Go/Node tests; publishing credentials stay in
the release-only workflow. No hardware throughput or GPU test coverage is claimed.
