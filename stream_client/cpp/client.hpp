#ifndef RTLSTREAM_CLIENT_HPP
#define RTLSTREAM_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace rtlsdr {

constexpr uint32_t RTLSTREAM_MAGIC_REQ = 0x52543252;
constexpr uint32_t RTLSTREAM_MAGIC_IQ  = 0x52543249;
constexpr uint32_t RTLSTREAM_MAGIC_FFT = 0x52543246;
constexpr uint32_t RTLSTREAM_MAGIC_EVT = 0x52543245;
constexpr uint32_t RTLSTREAM_MAX_IQ_COMPONENTS = 262144;
constexpr uint32_t RTLSTREAM_MAX_FFT_BINS = 65536;

enum class EventType : uint32_t { FrequencyChange = 1, StreamEnd = 2 };

class StreamEvent : public std::exception {
public:
	EventType type;
	uint64_t freq;
	StreamEvent(EventType type, uint64_t freq) : type(type), freq(freq) {}
	const char *what() const noexcept override { return "RTLSTREAM/2 event"; }
};

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
	StreamClient(const StreamClient &) = delete;
	StreamClient &operator=(const StreamClient &) = delete;

	void connect(const std::string &host, int port);
	void close();

	void request(uint64_t freq, uint64_t rate,
	             uint64_t bandwidth, uint8_t mode);

	iq_hdr readIQ(std::vector<int16_t> &samples);
	fft_hdr readFFT(std::vector<float> &power);

private:
	int fd_;
	bool requested_;

	static void put32(uint8_t *buf, uint32_t val);
	static void put64(uint8_t *buf, uint64_t val);
	static uint32_t get32(const uint8_t *buf);
	static uint64_t get64(const uint8_t *buf);

	void recvAll(uint8_t *buf, size_t len);
	void sendAll(const uint8_t *buf, size_t len);
	void readHeader(uint8_t *buf, uint32_t magic);
	void fail(const char *message);
};

} // namespace rtlsdr

#endif
