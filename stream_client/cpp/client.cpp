#include "client.hpp"
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

namespace rtlsdr {

StreamClient::StreamClient() : fd_(-1) {}

StreamClient::~StreamClient()
{
	close();
}

void StreamClient::connect(const std::string &host, int port)
{
	struct hostent *he = gethostbyname(host.c_str());
	if (!he)
		throw std::runtime_error("gethostbyname failed");

	fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (fd_ < 0)
		throw std::runtime_error("socket creation failed");

	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	std::memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

	if (::connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		::close(fd_);
		fd_ = -1;
		throw std::runtime_error("connect failed");
	}
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
		if (n <= 0)
			throw std::runtime_error("recv failed");
		buf += n;
		left -= (size_t)n;
	}
}

void StreamClient::sendAll(const uint8_t *buf, size_t len)
{
	size_t left = len;
	while (left > 0) {
		ssize_t n = send(fd_, buf, left, 0);
		if (n <= 0)
			throw std::runtime_error("send failed");
		buf += n;
		left -= (size_t)n;
	}
}

void StreamClient::request(uint64_t freq, uint64_t rate,
                           uint64_t bandwidth, uint8_t mode)
{
	uint8_t buf[29];
	put32(buf, RTLSTREAM_MAGIC_REQ);
	put64(buf + 4,  freq);
	put64(buf + 12, rate);
	put64(buf + 20, bandwidth);
	buf[28] = mode;
	sendAll(buf, 29);
}

iq_hdr StreamClient::readIQ(std::vector<int16_t> &samples)
{
	uint8_t hbuf[32];
	recvAll(hbuf, 32);

	iq_hdr hdr;
	hdr.magic    = get32(hbuf);
	hdr.freq     = get64(hbuf + 4);
	hdr.rate     = get64(hbuf + 12);
	hdr.seq      = get64(hbuf + 20);
	hdr.nsamples = get32(hbuf + 28);

	if (hdr.magic != RTLSTREAM_MAGIC_IQ)
		throw std::runtime_error("bad I/Q frame magic");

	size_t needed = (size_t)hdr.nsamples * sizeof(int16_t);
	if (samples.size() < (size_t)hdr.nsamples)
		samples.resize((size_t)hdr.nsamples);
	recvAll(reinterpret_cast<uint8_t *>(samples.data()), needed);

	return hdr;
}

fft_hdr StreamClient::readFFT(std::vector<float> &power)
{
	uint8_t hbuf[32];
	recvAll(hbuf, 32);

	fft_hdr hdr;
	hdr.magic = get32(hbuf);
	hdr.freq  = get64(hbuf + 4);
	hdr.rate  = get64(hbuf + 12);
	hdr.seq   = get64(hbuf + 20);
	hdr.bins  = get32(hbuf + 28);

	if (hdr.magic != RTLSTREAM_MAGIC_FFT)
		throw std::runtime_error("bad FFT frame magic");

	if ((size_t)hdr.bins > power.size())
		power.resize((size_t)hdr.bins);

	size_t nbytes = (size_t)hdr.bins * sizeof(float);
	recvAll(reinterpret_cast<uint8_t *>(power.data()), nbytes);

	return hdr;
}

} // namespace rtlsdr
