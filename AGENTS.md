# rtl-sdr fork: development plan

## Overview

This fork builds on Osmocom's rtl-sdr (v2.0.2+) with two major additions:

1. **Device listing API** (`rtlsdr_device_enumerate` + `rtl_list` utility) — *complete*
2. **rtl_stream** — channelized SDR server that captures full-bandwidth I/Q from a single RTL-SDR dongle and serves narrowband channel streams to multiple TCP clients

---

## 1. Device listing API (DONE)

### Library API (`include/rtl-sdr.h`, `src/librtlsdr.c`)

```c
typedef int (*rtlsdr_device_cb)(int index, const char *name,
    const char *manufact, const char *product, const char *serial, void *ctx);

RTLSDR_API int rtlsdr_device_enumerate(rtlsdr_device_cb cb, void *ctx);
```

- Single USB enumeration pass: `libusb_init` → `get_device_list` → `find_known_device` → `libusb_open` → `rtlsdr_get_usb_strings` → invoke callback
- Returns number of devices found
- Callback returns 0 to continue, non-0 to stop

### Utility (`src/rtl_list.c`)

```
rtl_list                    # text output
rtl_list -j                 # compact JSON
rtl_list -j -p              # pretty-printed JSON
```

---

## 2. rtl_stream — channelized SDR server

### Concept

Single RTL-SDR dongle captures the full bandwidth (up to ~3.2 MSPS). Each TCP client requests a narrowband channel (e.g., 25 kHz, 200 kHz, 1 MHz) centered at a specified frequency. The server NCO-mixes, decimates, and filters per client. An optional FFT stream provides wideband power spectra.

### Architecture

```
┌─────────────────┐     ┌──────────────┐     ┌──────────────────┐
│  RTL2832 USB     │────▶│  Ring Buffer  │────▶│  I/Q Listener    │
│  (async callback) │     │  (lock-free)  │     │  (port -p, TCP)  │
└─────────────────┘     └──────┬───────┘     └──────────────────┘
                               │
                    ┌──────────┴──────────┐
                    │  Client Manager      │
                    │  (per-client DSP)    │
                    └──────────┬──────────┘
                               │
                    ┌──────────┴──────────┐
                    │  DSP Pipeline:       │
                    │  NCO → CIC → FIR     │
                    └──────────────────────┘

┌─────────────────┐     ┌──────────────────┐
│  FFT Engine      │────▶│  FFT Listener     │
│  (VkFFT / CPU)   │     │  (port -P, TCP)   │
└─────────────────┘     └──────────────────┘
```

### Design decisions

| Decision | Choice |
|---|---|
| Project name | `rtl_stream` (in this repo) |
| Window function | Blackman-Harris (fixed) |
| I/Q frame timing | Push-based on USB callback (like rtl_tcp) |
| Ports | Separate: I/Q (`-p`), FFT (`-P`) |
| CLI style | Consistent with rtl_tcp conventions |
| Dongle identifier | `-i <id>` for multi-dongle |
| FFT acceleration | VkFFT (optional), CPU fallback |

### Protocol: RTLSTREAM/1.0

Binary protocol over TCP. Client sends a request frame specifying:
- Center frequency (Hz)
- Sample rate (Hz)
- Channel bandwidth (Hz)
- Mode (I/Q or FFT)

Server responds with continuous frames:
- **I/Q**: Header (magic, freq, rate, seq) + int16 I/Q samples
- **FFT**: Header (magic, freq, rate, seq, bins) + float32 power spectrum

### Files to create

| File | Purpose |
|---|---|
| `src/rtl_stream.c` | Main entry, CLI parsing, capture thread, init |
| `src/rtl_stream_server.c` | Dual-port accept loop + client list management |
| `src/rtl_stream_server.h` | Server interface |
| `src/rtl_stream_proto.c` | RTLSTREAM/1.0 protocol encode/decode |
| `src/rtl_stream_proto.h` | Protocol constants + structs |
| `src/rtl_dsp.c` | Per-client DSP: NCO, CIC decimate, FIR filter |
| `src/rtl_dsp.h` | DSP pipeline interface |
| `src/rtl_ring.c` | Lock-free SPSC ring buffer |
| `src/rtl_ring.h` | Ring buffer interface |
| `src/fft/fft_backend.h` | Unified FFT interface |
| `src/fft/fft_cpu.c` | CPU-based FFT (simple DFT or fftwf) |
| `src/fft/fft_vkfft.c` | GPU-accelerated FFT via VkFFT |

### CLI

```
rtl_stream -f 100.5M -s 2.4M [-p 1234] [-P 1235] [-i 0] [--list]
```

| Flag | Default | Description |
|---|---|---|
| `-f` | (required) | Center frequency |
| `-s` | 2.4M | Sample rate |
| `-p` | 1234 | I/Q TCP port |
| `-P` | 0 | FFT TCP port (0 = disable) |
| `-i` | 0 | Device index |
| `--list` | — | List dongles + configured ports/identifiers and exit |
| `-h` | — | Help |

### Task breakdown

| Priority | Task | Dependencies |
|---|---|---|
| P0 | Ring buffer (`rtl_ring.c/h`) | None |
| P0 | DSP pipeline (`rtl_dsp.c/h`) — NCO + CIC | None |
| P0 | rtl_stream main + USB capture thread | Ring buffer |
| P0 | I/Q server (single-port accept + stream) | Ring buffer, DSP |
| P0 | Protocol (`rtl_stream_proto.c/h`) | None |
| P1 | FIR filter in DSP pipeline | DSP pipeline |
| P1 | FFT engine, CPU backend | Ring buffer |
| P1 | FFT TCP server (dual-port) | FFT engine |
| P2 | `--list` flag (enumerate + show ports) | Device listing API (done) |
| P2 | VkFFT backend | FFT engine |
| P2 | Multi-dongle support (`-i`) | None |

---

## 3. Build system

### CMake (`src/CMakeLists.txt`)

```cmake
add_executable(rtl_stream
    rtl_stream.c
    rtl_stream_server.c
    rtl_stream_proto.c
    rtl_dsp.c
    rtl_ring.c
    fft/fft_cpu.c
    fft/fft_vkfft.c
)
target_link_libraries(rtl_stream rtlsdr_static m)
# Optional: find_package(VkFFT) for GPU FFT
```

### Dependencies

| Library | Required | Notes |
|---|---|---|
| libusb-1.0 | Yes | Already a dependency |
| pthreads | Yes | Already a dependency |
| libm | Yes | Standard math |
| VkFFT | No | Optional GPU acceleration |

---

## 4. Release automation

### GitHub Actions (`.github/workflows/build.yml`)

Trigger: `release: [published]`

Already configured:
- Matrix: gcc + clang
- Build + package tarballs (libraries + headers + binaries)
- Docker multi-stage Alpine build
- Push to `ghcr.io/gbozo/rtl-sdr:<tag>` and `:latest`

New: On next release, `rtl_list` (and later `rtl_stream`) are automatically included in the build matrix and tarballs/Docker image — no workflow changes needed (CMake handles it).

---

## 5. Implementation order

1. ~~Device listing API + rtl_list~~ (DONE)
2. Ring buffer — lock-free SPSC
3. Basic rtl_stream — capture thread + single I/Q client
4. Protocol definition
5. DSP pipeline — NCO + CIC
6. DSP pipeline — FIR filter
7. FFT engine (CPU first)
8. Dual-port server (I/Q + FFT)
9. `--list` flag
10. VkFFT backend (optional)
11. Multi-dongle (`-i` flag)
