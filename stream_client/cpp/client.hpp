#ifndef RTLSTREAM_CLIENT_HPP
#define RTLSTREAM_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace rtlsdr {

constexpr uint32_t RTLSTREAM_MAGIC_REQ = 0x52545352;
constexpr uint32_t RTLSTREAM_MAGIC_IQ  = 0x52545349;
constexpr uint32_t RTLSTREAM_MAGIC_FFT = 0x52545346;
constexpr uint32_t RTLSTREAM_MAGIC_EVT = 0x52545345;

constexpr uint8_t RTLSTREAM_MODE_IQ  = 0;
constexpr uint8_t RTLSTREAM_MODE_FFT = 1;

struct iq_hdr {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t seq;
	uint32_t nsamples;
};

struct fft_hdr {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t seq;
	uint32_t bins;
};

class StreamClient {
public:
	StreamClient();
	~StreamClient();

	void connect(const std::string &host, int port);
	void close();

	void request(uint64_t freq, uint64_t rate,
	             uint64_t bandwidth, uint8_t mode);

	iq_hdr readIQ(std::vector<int16_t> &samples);
	fft_hdr readFFT(std::vector<float> &power);

private:
	int fd_;

	static void put32(uint8_t *buf, uint32_t val);
	static void put64(uint8_t *buf, uint64_t val);
	static uint32_t get32(const uint8_t *buf);
	static uint64_t get64(const uint8_t *buf);

	void recvAll(uint8_t *buf, size_t len);
	void sendAll(const uint8_t *buf, size_t len);
};

} // namespace rtlsdr

#endif
