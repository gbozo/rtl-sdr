#include "client.hpp"
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <limits>
#include <cstdio>

namespace rtlsdr {

StreamClient::StreamClient() : fd_(-1), requested_(false) {}

StreamClient::~StreamClient()
{
	close();
}

void StreamClient::connect(const std::string &host, int port)
{
	close();
	requested_ = false;
	if (port < 1 || port > 65535)
		throw std::invalid_argument("invalid port");
	struct addrinfo hints = {}, *addresses;
	char service[6];
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	std::snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host.c_str(), service, &hints, &addresses) != 0)
		throw std::runtime_error("getaddrinfo failed");
	for (struct addrinfo *address = addresses; address; address = address->ai_next) {
		fd_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (fd_ < 0)
			continue;
#ifdef SO_NOSIGPIPE
		int enabled = 1;
		setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
		if (::connect(fd_, address->ai_addr, address->ai_addrlen) == 0)
			break;
		::close(fd_);
		fd_ = -1;
	}
	freeaddrinfo(addresses);
	if (fd_ < 0)
		throw std::runtime_error("connect failed");
}

void StreamClient::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

void StreamClient::put32(uint8_t *buf, uint32_t val)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)(val);
}

void StreamClient::put64(uint8_t *buf, uint64_t val)
{
	put32(buf,     (uint32_t)(val >> 32));
	put32(buf + 4, (uint32_t)(val));
}

uint32_t StreamClient::get32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) |
	       ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8)  |
	       ((uint32_t)buf[3]);
}

uint64_t StreamClient::get64(const uint8_t *buf)
{
	return ((uint64_t)get32(buf) << 32) | get32(buf + 4);
}

void StreamClient::recvAll(uint8_t *buf, size_t len)
{
	size_t left = len;
	while (left > 0) {
		ssize_t n = recv(fd_, buf, left, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			fail("recv failed or truncated frame");
		buf += n;
		left -= (size_t)n;
	}
}

void StreamClient::sendAll(const uint8_t *buf, size_t len)
{
	size_t left = len;
	while (left > 0) {
		ssize_t n;
#ifdef MSG_NOSIGNAL
		n = send(fd_, buf, left, MSG_NOSIGNAL);
#else
		n = send(fd_, buf, left, 0);
#endif
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			fail("send failed");
		buf += n;
		left -= (size_t)n;
	}
}

void StreamClient::request(uint64_t freq, uint64_t rate,
                           uint64_t bandwidth, uint8_t mode)
{
	uint8_t buf[29];
	if (fd_ < 0 || requested_ || (mode != RTLSTREAM_MODE_IQ && mode != RTLSTREAM_MODE_FFT) ||
	    (mode == RTLSTREAM_MODE_FFT && (freq || rate || bandwidth)))
		throw std::invalid_argument("invalid or repeated request");
	requested_ = true;
	put32(buf, RTLSTREAM_MAGIC_REQ);
	put64(buf + 4,  freq);
	put64(buf + 12, rate);
	put64(buf + 20, bandwidth);
	buf[28] = mode;
	sendAll(buf, 29);
}

void StreamClient::fail(const char *message)
{
	close();
	throw std::runtime_error(message);
}

void StreamClient::readHeader(uint8_t *buf, uint32_t magic)
{
	recvAll(buf, 4);
	if (get32(buf) == RTLSTREAM_MAGIC_EVT) {
		recvAll(buf + 4, 4);
		uint32_t type = get32(buf + 4);
		uint64_t freq = 0;
		if (type == static_cast<uint32_t>(EventType::FrequencyChange)) {
			recvAll(buf + 8, 8);
			freq = get64(buf + 8);
		} else if (type == static_cast<uint32_t>(EventType::StreamEnd)) {
			close();
		} else {
			fail("unknown event type");
		}
		throw StreamEvent(static_cast<EventType>(type), freq);
	}
	if (get32(buf) != magic)
		fail("bad RTLSTREAM/2 frame magic");
	recvAll(buf + 4, 28);
}

iq_hdr StreamClient::readIQ(std::vector<int16_t> &samples)
{
	uint8_t hbuf[32];
	readHeader(hbuf, RTLSTREAM_MAGIC_IQ);

	iq_hdr hdr;
	hdr.magic    = get32(hbuf);
	hdr.freq     = get64(hbuf + 4);
	hdr.rate     = get64(hbuf + 12);
	hdr.seq      = get64(hbuf + 20);
	hdr.nsamples = get32(hbuf + 28);

	if (hdr.nsamples > RTLSTREAM_MAX_IQ_COMPONENTS || (hdr.nsamples & 1))
		fail("invalid I/Q component count");

	size_t needed = (size_t)hdr.nsamples * sizeof(int16_t);
	samples.resize((size_t)hdr.nsamples);
	recvAll(reinterpret_cast<uint8_t *>(samples.data()), needed);
	for (size_t i = 0; i < samples.size(); ++i) {
		const uint8_t *p = reinterpret_cast<const uint8_t *>(&samples[i]);
		uint32_t value = (static_cast<uint32_t>(p[0]) << 8) | p[1];
		samples[i] = static_cast<int16_t>(value < 32768 ? static_cast<int32_t>(value) : static_cast<int32_t>(value) - 65536);
	}

	return hdr;
}

fft_hdr StreamClient::readFFT(std::vector<float> &power)
{
	uint8_t hbuf[32];
	readHeader(hbuf, RTLSTREAM_MAGIC_FFT);

	fft_hdr hdr;
	hdr.magic = get32(hbuf);
	hdr.freq  = get64(hbuf + 4);
	hdr.rate  = get64(hbuf + 12);
	hdr.seq   = get64(hbuf + 20);
	hdr.bins  = get32(hbuf + 28);

	if (hdr.bins < 2 || hdr.bins > RTLSTREAM_MAX_FFT_BINS)
		fail("invalid FFT bin count");

	static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
	              "RTLSTREAM/2 requires IEEE-754 binary32");
	power.resize((size_t)hdr.bins);

	size_t nbytes = (size_t)hdr.bins * sizeof(float);
	recvAll(reinterpret_cast<uint8_t *>(power.data()), nbytes);
	for (size_t i = 0; i < power.size(); ++i) {
		uint32_t bits = get32(reinterpret_cast<const uint8_t *>(&power[i]));
		std::memcpy(&power[i], &bits, sizeof(bits));
	}

	return hdr;
}

} // namespace rtlsdr
