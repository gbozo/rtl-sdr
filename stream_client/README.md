# rtl_stream Client Libraries

Client libraries for the RTLSTREAM/1.0 protocol used by `rtl_stream` — a
channelized SDR server that captures full-bandwidth I/Q from an RTL-SDR dongle
and serves narrowband channel streams to multiple TCP clients.

## Protocol

All multi-byte values are **big-endian** (network byte order).

### Request (client → server, 29 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | Magic `0x52545352` (`"RTSR"`) |
| 4      | 8    | Center frequency (Hz) |
| 12     | 8    | Sample rate (Hz) |
| 20     | 8    | Channel bandwidth (Hz) |
| 28     | 1    | Mode: `0` = I/Q, `1` = FFT |

### I/Q Frame (server → client)

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | Magic `0x52545349` (`"RTSI"`) |
| 4      | 8    | Center frequency (Hz) |
| 12     | 8    | Sample rate (Hz) |
| 20     | 8    | Sequence number |
| 24     | N/A  | Interleaved `int16` I/Q samples |

### FFT Frame (server → client)

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | Magic `0x52545346` (`"RTSF"`) |
| 4      | 8    | Center frequency (Hz) |
| 12     | 8    | Sample rate (Hz) |
| 20     | 8    | Sequence number |
| 24     | 4    | Number of bins |
| 28     | N/A  | `float32` power spectrum values |

### Event Frame (server → client)

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | Magic `0x52545345` (`"RTSE"`) |
| 4      | 4    | Event type |
| 8      | 8    | Frequency (for frequency change events) |

Event types: `1` = frequency change, `2` = stream end.

## Language Support

| Language | Directory | Dependencies |
|----------|-----------|--------------|
| C        | `c/`      | POSIX sockets |
| C++      | `cpp/`    | POSIX sockets |
| Go       | `go/`     | None (stdlib) |
| Node.js  | `node/`   | None (stdlib) |

## Usage Examples

### C

```c
#include "client.h"

rtlsdr_stream_ctx *ctx = rtlsdr_stream_connect("127.0.0.1", 1234);
rtlsdr_stream_request(ctx, 100500000ULL, 2400000ULL, 200000ULL, 0);

struct rtlsdr_stream_iq_hdr hdr;
int16_t samples[4096];
rtlsdr_stream_read_iq(ctx, &hdr, samples, 2048);
```

Build:
```sh
cc -o example example.c client.c
```

### C++

```cpp
#include "client.hpp"

rtlsdr::StreamClient client;
client.connect("127.0.0.1", 1234);
client.request(100500000ULL, 2400000ULL, 200000ULL, rtlsdr::RTLSTREAM_MODE_IQ);

std::vector<int16_t> samples(4096);
auto hdr = client.readIQ(samples);
```

Build:
```sh
c++ -o example example.cpp client.cpp
```

### Go

```go
import "rtlstream"

client, _ := rtlstream.Connect("127.0.0.1", 1234)
client.Request(100500000, 2400000, 200000, rtlstream.ModeIQ)

samples := make([]int16, 4096)
hdr, _ := client.ReadIQ(samples)
```

Run:
```sh
go run example/main.go 127.0.0.1 1234 100500000
```

### Node.js

```js
const { RtlsdrStreamClient, MODE_IQ } = require('./client');

const client = new RtlsdrStreamClient();
await client.connect('127.0.0.1', 1234);
await client.request(100500000, 2400000, 200000, MODE_IQ);

const { hdr, samples } = await client.readIQ(2048);
```

Run:
```sh
node example.js 127.0.0.1 1234 100500000
```

## License

Same as rtl-sdr: GPL v2 (or later).
