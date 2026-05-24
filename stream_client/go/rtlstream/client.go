package rtlstream

import (
	"encoding/binary"
	"fmt"
	"math"
	"net"
)

const (
	MagicReq = 0x52545352
	MagicIQ  = 0x52545349
	MagicFFT = 0x52545346
	MagicEVT = 0x52545345

	ModeIQ  = 0
	ModeFFT = 1
)

type IQHdr struct {
	Magic    uint32
	Freq     uint64
	Rate     uint64
	Seq      uint64
	NSamples uint32
}

type FFTHdr struct {
	Magic uint32
	Freq  uint64
	Rate  uint64
	Seq   uint64
	Bins  uint32
}

type Client struct {
	conn net.Conn
}

func Connect(host string, port int) (*Client, error) {
	addr := fmt.Sprintf("%s:%d", host, port)
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	return &Client{conn: conn}, nil
}

func (c *Client) Close() {
	if c.conn != nil {
		c.conn.Close()
	}
}

func (c *Client) Request(freq, rate, bandwidth uint64, mode uint8) error {
	buf := make([]byte, 29)
	binary.BigEndian.PutUint32(buf[0:4], MagicReq)
	binary.BigEndian.PutUint64(buf[4:12], freq)
	binary.BigEndian.PutUint64(buf[12:20], rate)
	binary.BigEndian.PutUint64(buf[20:28], bandwidth)
	buf[28] = mode
	_, err := c.conn.Write(buf)
	return err
}

func (c *Client) ReadIQ(samples []int16) (*IQHdr, int, error) {
	hbuf := make([]byte, 32)
	if _, err := recvFull(c.conn, hbuf); err != nil {
		return nil, 0, err
	}

	hdr := &IQHdr{
		Magic:    binary.BigEndian.Uint32(hbuf[0:4]),
		Freq:     binary.BigEndian.Uint64(hbuf[4:12]),
		Rate:     binary.BigEndian.Uint64(hbuf[12:20]),
		Seq:      binary.BigEndian.Uint64(hbuf[20:28]),
		NSamples: binary.BigEndian.Uint32(hbuf[28:32]),
	}

	if hdr.Magic != MagicIQ {
		return nil, 0, fmt.Errorf("bad I/Q frame magic: 0x%08x", hdr.Magic)
	}

	n := int(hdr.NSamples)
	nbytes := n * 2
	sbuf := make([]byte, nbytes)
	if _, err := recvFull(c.conn, sbuf); err != nil {
		return nil, 0, err
	}
	nw := n
	if nw > len(samples) {
		nw = len(samples)
	}
	for i := 0; i < nw; i++ {
		samples[i] = int16(binary.BigEndian.Uint16(sbuf[i*2:]))
	}

	return hdr, nw, nil
}

func (c *Client) ReadFFT(power []float32) (*FFTHdr, []float32, error) {
	hbuf := make([]byte, 32)
	if _, err := recvFull(c.conn, hbuf); err != nil {
		return nil, nil, err
	}

	hdr := &FFTHdr{
		Magic: binary.BigEndian.Uint32(hbuf[0:4]),
		Freq:  binary.BigEndian.Uint64(hbuf[4:12]),
		Rate:  binary.BigEndian.Uint64(hbuf[12:20]),
		Seq:   binary.BigEndian.Uint64(hbuf[20:28]),
		Bins:  binary.BigEndian.Uint32(hbuf[28:32]),
	}

	if hdr.Magic != MagicFFT {
		return nil, nil, fmt.Errorf("bad FFT frame magic: 0x%08x", hdr.Magic)
	}

	nb := int(hdr.Bins)
	if cap(power) < nb {
		power = make([]float32, nb)
	} else {
		power = power[:nb]
	}
	nbytes := nb * 4
	fbuf := make([]byte, nbytes)
	if _, err := recvFull(c.conn, fbuf); err != nil {
		return nil, nil, err
	}
	for i := 0; i < nb; i++ {
		power[i] = math.Float32frombits(binary.BigEndian.Uint32(fbuf[i*4:]))
	}

	return hdr, power, nil
}

func recvFull(conn net.Conn, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := conn.Read(buf[total:])
		if err != nil {
			return total, err
		}
		total += n
	}
	return total, nil
}
