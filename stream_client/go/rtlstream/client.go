package rtlstream

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"sync"
)

const (
	MagicReq = 0x52543252
	MagicIQ  = 0x52543249
	MagicFFT = 0x52543246
	MagicEVT = 0x52543245

	ModeIQ          = 0
	ModeFFT         = 1
	MaxIQComponents = 262144
	MaxFFTBins      = 65536
)

type EventType uint32

const (
	FrequencyChange EventType = 1
	StreamEnd       EventType = 2
)

type Event struct {
	Type EventType
	Freq uint64
}

func (e *Event) Error() string { return fmt.Sprintf("RTLSTREAM/2 event %d", e.Type) }

var ErrCapacity = errors.New("insufficient component capacity; frame drained")
var ErrProtocol = errors.New("invalid RTLSTREAM/2 frame")

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
	conn      net.Conn
	readMu    sync.Mutex
	requestMu sync.Mutex
	requested bool
}

func Connect(host string, port int) (*Client, error) {
	addr := net.JoinHostPort(host, fmt.Sprint(port))
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
	c.requestMu.Lock()
	defer c.requestMu.Unlock()
	if c.requested || (mode != ModeIQ && mode != ModeFFT) ||
		(mode == ModeFFT && (freq != 0 || rate != 0 || bandwidth != 0)) {
		return errors.New("invalid or repeated request")
	}
	c.requested = true
	buf := make([]byte, 29)
	binary.BigEndian.PutUint32(buf[0:4], MagicReq)
	binary.BigEndian.PutUint64(buf[4:12], freq)
	binary.BigEndian.PutUint64(buf[12:20], rate)
	binary.BigEndian.PutUint64(buf[20:28], bandwidth)
	buf[28] = mode
	for len(buf) > 0 {
		n, err := c.conn.Write(buf)
		if err != nil {
			c.Close()
			return err
		}
		if n == 0 {
			c.Close()
			return io.ErrShortWrite
		}
		buf = buf[n:]
	}
	return nil
}

func (c *Client) readFull(buf []byte) error {
	_, err := io.ReadFull(c.conn, buf)
	if err != nil {
		c.Close()
	}
	return err
}

func (c *Client) protocolError() error { c.Close(); return ErrProtocol }

func (c *Client) readHeader(buf []byte, magic uint32) error {
	if err := c.readFull(buf[:4]); err != nil {
		return err
	}
	got := binary.BigEndian.Uint32(buf[:4])
	if got == MagicEVT {
		if err := c.readFull(buf[4:8]); err != nil {
			return err
		}
		event := &Event{Type: EventType(binary.BigEndian.Uint32(buf[4:8]))}
		switch event.Type {
		case FrequencyChange:
			if err := c.readFull(buf[8:16]); err != nil {
				return err
			}
			event.Freq = binary.BigEndian.Uint64(buf[8:16])
		case StreamEnd:
			c.Close()
		default:
			return c.protocolError()
		}
		return event
	}
	if got != magic {
		return c.protocolError()
	}
	return c.readFull(buf[4:32])
}

func (c *Client) ReadIQ(samples []int16) (*IQHdr, int, error) {
	c.readMu.Lock()
	defer c.readMu.Unlock()
	hbuf := make([]byte, 32)
	if err := c.readHeader(hbuf, MagicIQ); err != nil {
		return nil, 0, err
	}

	hdr := &IQHdr{
		Magic:    binary.BigEndian.Uint32(hbuf[0:4]),
		Freq:     binary.BigEndian.Uint64(hbuf[4:12]),
		Rate:     binary.BigEndian.Uint64(hbuf[12:20]),
		Seq:      binary.BigEndian.Uint64(hbuf[20:28]),
		NSamples: binary.BigEndian.Uint32(hbuf[28:32]),
	}

	if hdr.NSamples > MaxIQComponents || hdr.NSamples&1 != 0 {
		return nil, 0, c.protocolError()
	}

	n := int(hdr.NSamples)
	nbytes := n * 2
	if n > len(samples) {
		if _, err := io.CopyN(io.Discard, c.conn, int64(nbytes)); err != nil {
			c.Close()
			return hdr, 0, err
		}
		return hdr, 0, ErrCapacity
	}
	sbuf := make([]byte, nbytes)
	if err := c.readFull(sbuf); err != nil {
		return nil, 0, err
	}
	for i := 0; i < n; i++ {
		samples[i] = int16(binary.BigEndian.Uint16(sbuf[i*2:]))
	}

	return hdr, n, nil
}

func (c *Client) ReadFFT(power []float32) (*FFTHdr, []float32, error) {
	c.readMu.Lock()
	defer c.readMu.Unlock()
	hbuf := make([]byte, 32)
	if err := c.readHeader(hbuf, MagicFFT); err != nil {
		return nil, nil, err
	}

	hdr := &FFTHdr{
		Magic: binary.BigEndian.Uint32(hbuf[0:4]),
		Freq:  binary.BigEndian.Uint64(hbuf[4:12]),
		Rate:  binary.BigEndian.Uint64(hbuf[12:20]),
		Seq:   binary.BigEndian.Uint64(hbuf[20:28]),
		Bins:  binary.BigEndian.Uint32(hbuf[28:32]),
	}

	if hdr.Bins < 2 || hdr.Bins > MaxFFTBins {
		return nil, nil, c.protocolError()
	}

	nb := int(hdr.Bins)
	if cap(power) < nb {
		power = make([]float32, nb)
	} else {
		power = power[:nb]
	}
	nbytes := nb * 4
	fbuf := make([]byte, nbytes)
	if err := c.readFull(fbuf); err != nil {
		return nil, nil, err
	}
	for i := 0; i < nb; i++ {
		power[i] = math.Float32frombits(binary.BigEndian.Uint32(fbuf[i*4:]))
	}

	return hdr, power, nil
}
