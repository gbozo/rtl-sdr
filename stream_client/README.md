# rtl_stream clients — RTLSTREAM/2

**Breaking alpha wire revision.** These clients and the server speak only
RTLSTREAM/2. Version 1 magics and host-endian payloads are rejected; there is no
autodetection or negotiation. Rebuild both endpoints together.
The checked-in Linux Go executables are legacy artifacts, not rebuilt v2
clients; use the source-based build/run commands below instead.

## Wire format

Every integer and payload component is **big-endian**, without struct padding.
Frequencies, rates, bandwidths and sequence numbers are unsigned 64-bit values.
Float payloads use IEEE-754 binary32 bit patterns in network byte order.

One 29-byte request is allowed per TCP connection:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | `0x52543252` (`RT2R`) |
| 4 | 8 | Center frequency, Hz |
| 12 | 8 | Requested output sample rate, Hz |
| 20 | 8 | Channel bandwidth, Hz |
| 28 | 1 | Mode: I/Q = 0, FFT = 1 |

Use the I/Q listener for mode 0, the FFT listener for mode 1. **FFT requests
must set frequency, rate and bandwidth to zero**: FFT always describes the full
capture band. Reconnect to request a different channel.

I/Q supports **exact integer decimation**, not arbitrary resampling:
`capture_rate % output_rate == 0`, with ratio 1 through 4096. For filtered
channels, bandwidth 0 defaults to 80% of output rate; otherwise
`0 < bandwidth <= 0.8 * output_rate`. The channel must fit within the capture
band. Raw bypass is only available at zero frequency offset, with
`bandwidth == output_rate == capture_rate`.

I/Q and FFT data headers are both 32 bytes:

| Offset | Bytes | I/Q | FFT |
|---|---|---|---|
| 0 | 4 | `0x52543249` (`RT2I`) | `0x52543246` (`RT2F`) |
| 4 | 8 | Channel center, Hz | Capture center, Hz |
| 12 | 8 | Output sample rate, Hz | **Capture sample rate**, Hz |
| 20 | 8 | Sequence number | Sequence number |
| 28 | 4 | Scalar int16 component count | Float32 bin count |
| 32 | count × 2 / count × 4 | Interleaved signed I,Q | Linear power |

I/Q count must be even, from 0 through **262144 components** (131072 complex
samples). FFT count is **2 through 65536 bins**. Readers validate counts before
multiplication or allocation. Invalid magics, counts, unknown events and
truncated frames close the connection; never attempt to resynchronize.

USB unsigned 8-bit components are centered at 127.5. I/Q output has a scale
factor of **256**, quantized/saturated to int16 (raw endpoints are -32640 and
32640). Divide received I/Q components by 256 to recover centered ADC units.
Filtered channels have unity passband gain apart from filter transients.

FFT power is the averaged squared magnitude of the **unnormalized** forward
FFT of Blackman–Harris-windowed, centered ADC samples. It is linear ADC-unit
power, not dB, dBFS, calibrated watts or a power spectral density; no FFT-length
or window-gain normalization is applied. Bins are in **native unshifted order**:
DC first, then positive frequencies, then negative frequencies. Bin spacing is
`header.rate / bins`; for even `N`, bin `k` has offset
`(k < N/2 ? k : k-N) * rate/N`. Apply fftshift locally for a centered display.

Events can appear before or between data frames on either port:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | `0x52543245` (`RT2E`) |
| 4 | 4 | Type: frequency change = 1, stream end = 2 |
| 8 | 8 | New frequency, **only** for type 1 |

Frequency events are 16 bytes, end events are 8 bytes. Read the magic first,
not a 32-byte data header. Stream end is terminal and returned once as a typed
event. A connection may also end through ordinary EOF or an I/O error.

## Client APIs

All examples request a 240 kS/s, 192 kHz channel (compatible with a 2.4 MS/s
capture). Each client library uses only its language runtime/POSIX sockets.

### C (`c/client.h`, `c/client.c`)

`rtlsdr_stream_read_iq(ctx, &hdr, samples, max_components)` takes capacity in
**scalar components, not complex pairs or bytes**. FFT capacity is in bins.
Return values:

- `RTLSTREAM_READ_OK` (0): complete data frame decoded.
- `RTLSTREAM_READ_EVENT` (1): inspect `rtlsdr_stream_last_event(ctx)`; data
  header/payload are not populated for an event.
- `RTLSTREAM_ERROR_CAPACITY` (-2): a legal frame did not fit. Its entire payload
  was drained, the header reports the required count, and the next read starts
  at the next frame. No partial samples are returned.
- `RTLSTREAM_ERROR` (-1): invalid argument, protocol failure or I/O failure.

Allocate `int16_t samples[RTLSTREAM_MAX_IQ_COMPONENTS]` to accept all legal I/Q
frames. See `c/example.c` for event handling. Do not concurrently access a C
client context. Build: `cc -std=c11 c/example.c c/client.c -o example-c`.

### C++ (`cpp/client.hpp`, `cpp/client.cpp`)

`readIQ(vector<int16_t>&)` and `readFFT(vector<float>&)` resize the output to
the exact bounded frame count. Catch `rtlsdr::StreamEvent` **inside** the read
loop; its `type` is `EventType::FrequencyChange` or `EventType::StreamEnd`,
with a uint64 `freq`. Other errors throw standard exceptions. Instances are
noncopyable and must not be accessed concurrently.

Build: `c++ -std=c++11 cpp/example.cpp cpp/client.cpp -o example-cpp`.

### Go (`go/rtlstream`)

`ReadIQ(samples)` uses `len(samples)` as component capacity. An undersized
slice causes a full drain and returns `(header, 0, ErrCapacity)`; retry with a
larger slice for subsequent frames. `ReadFFT(power)` grows/reuses its slice
after validating the count. Both return `*Event` through the error result;
use `errors.As(err, &event)` before treating it as an error. Protocol errors
are `ErrProtocol`. Reads are serialized; request writes handle short writes.

From `go/`: `go run ./example 127.0.0.1 1234 100500000`.

### Node.js (`node/client.js`)

Await `request(...)` to finish writing. `readIQ()` / `readFFT()` return
`{hdr, samples}` / `{hdr, power}`, or `{event: StreamEvent}`. No caller-specified
count overrides the wire count. All uint64 fields, including event frequency,
remain **BigInt**. Requests accept BigInt or safe integer values; unsafe Number
inputs are rejected. Use explicit conversion only when precision loss is safe.

The socket stays in paused-readable mode; exact reads retain coalesced surplus
bytes and settle on EOF/error/close. Concurrent frame reads are rejected without
disturbing the pending read. Call `close()` to cancel; create a new client for a
new connection.

Run: `node node/example.js 127.0.0.1 1234 100500000`.

## Tests

Golden hex fixtures in `../tests/fixtures/` are shared across all languages.
They include signed int16 extrema, known float32 values, requests, events and
uint64 values above JavaScript's safe integer range. TCP/pipe tests cover
fragmentation, coalescing, capacity drain recovery, malformed counts and EOF.

From the repository root:

```sh
cc -std=c11 tests/test_proto.c src/rtl_stream_proto.c -o /path/to/test_proto
cc -std=c11 tests/test_client_c.c stream_client/c/client.c -o /path/to/test_client_c
c++ -std=c++11 tests/test_client_cpp.cpp stream_client/cpp/client.cpp -o /path/to/test_client_cpp
# Run the three executables, then:
(cd stream_client/go && go test -race ./...)
node stream_client/node/client.test.js
```

## License

Same as rtl-sdr: GPL v2 (or later).
