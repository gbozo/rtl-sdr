package rtlstream

import (
	"bytes"
	"encoding/hex"
	"errors"
	"io"
	"net"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"
)

const goldFreq = uint64(9007199254740993)
const goldSeq = uint64(0x0102030405060708)

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	text, err := os.ReadFile("../../../tests/fixtures/" + name + ".hex")
	if err != nil {
		t.Fatal(err)
	}
	data, err := hex.DecodeString(strings.Join(strings.Fields(string(text)), ""))
	if err != nil {
		t.Fatal(err)
	}
	return data
}

type shortWriter struct{ net.Conn }

func (c shortWriter) Write(p []byte) (int, error) {
	if len(p) > 3 {
		p = p[:3]
	}
	return c.Conn.Write(p)
}

func peer(t *testing.T, data []byte, fft, fragment bool) (*Client, <-chan error) {
	t.Helper()
	left, right := net.Pipe()
	left.SetDeadline(time.Now().Add(3 * time.Second))
	right.SetDeadline(time.Now().Add(3 * time.Second))
	c := &Client{conn: shortWriter{left}}
	t.Cleanup(c.Close)
	name := "request_iq"
	if fft {
		name = "request_fft"
	}
	expected := fixture(t, name)
	done := make(chan error, 1)
	go func() {
		defer right.Close()
		request := make([]byte, 29)
		if _, err := io.ReadFull(right, request); err != nil {
			done <- err
			return
		}
		if !bytes.Equal(request, expected) {
			done <- errors.New("request mismatch")
			return
		}
		for offset := 0; offset < len(data); {
			chunk := len(data) - offset
			if fragment && offset < 35 {
				chunk = 1
			}
			n, err := right.Write(data[offset : offset+chunk])
			if err != nil {
				done <- nil
				return
			}
			offset += n
		}
		done <- nil
	}()
	var err error
	if fft {
		err = c.Request(0, 0, 0, ModeFFT)
	} else {
		err = c.Request(goldFreq, 240000, 192000, ModeIQ)
	}
	if err != nil {
		t.Fatal(err)
	}
	if c.Request(0, 0, 0, ModeFFT) == nil {
		t.Fatal("repeated request accepted")
	}
	return c, done
}

func TestGoldenFrames(t *testing.T) {
	for _, fft := range []bool{false, true} {
		name := "iq"
		if fft {
			name = "fft"
		}
		t.Run(name, func(t *testing.T) {
			frame := fixture(t, name)
			data := append([]byte{}, frame...)
			data = append(data, fixture(t, "frequency")...)
			data = append(data, frame...)
			data = append(data, frame...)
			data = append(data, fixture(t, "end")...)
			c, done := peer(t, data, fft, true)
			samples := make([]int16, 6)
			for i := 0; i < 5; i++ {
				var err error
				if fft {
					var hdr *FFTHdr
					var power []float32
					hdr, power, err = c.ReadFFT(nil)
					if err == nil {
						if hdr.Freq != goldFreq || hdr.Seq != goldSeq || hdr.Rate != 2400000 || hdr.Bins != 4 {
							t.Fatal(hdr)
						}
						if !reflect.DeepEqual(power, []float32{0, 1, -2.5, .5}) {
							t.Fatal(power)
						}
					}
				} else {
					var hdr *IQHdr
					var n int
					capacity := samples
					if i == 0 {
						capacity = samples[:2]
					}
					hdr, n, err = c.ReadIQ(capacity)
					if i == 0 {
						if !errors.Is(err, ErrCapacity) || n != 0 || hdr.NSamples != 6 {
							t.Fatalf("capacity: %v %d %v", hdr, n, err)
						}
						continue
					}
					if err == nil {
						if hdr.Freq != goldFreq || hdr.Seq != goldSeq || hdr.Rate != 2400000 || n != 6 {
							t.Fatal(hdr, n)
						}
						if !reflect.DeepEqual(samples, []int16{-32768, -1, 0, 1, 256, 32767}) {
							t.Fatal(samples)
						}
					}
				}
				if i == 1 || i == 4 {
					var event *Event
					if !errors.As(err, &event) {
						t.Fatalf("expected event: %v", err)
					}
					if i == 1 && (event.Type != FrequencyChange || event.Freq != goldFreq) {
						t.Fatal(event)
					}
					if i == 4 && event.Type != StreamEnd {
						t.Fatal(event)
					}
				} else if err != nil {
					t.Fatal(err)
				}
			}
			if _, _, err := c.ReadIQ(samples); err == nil {
				t.Fatal("read after end")
			}
			if err := <-done; err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestInvalidFrames(t *testing.T) {
	for _, fft := range []bool{false, true} {
		name := "iq"
		if fft {
			name = "fft"
		}
		for kind := 0; kind < 7; kind++ {
			data := fixture(t, name)
			switch kind {
			case 0:
				data[2] = 0x53
			case 1:
				for i := 28; i < 32; i++ {
					data[i] = 0xff
				}
			case 2:
				for i := 28; i < 32; i++ {
					data[i] = 0
				}
				data[31] = 1
			case 3:
				data = data[:17]
			case 4:
				data = data[:len(data)-1]
			case 5:
				data = fixture(t, "frequency")
				data[7] = 99
			case 6:
				data = fixture(t, "frequency")[:15]
			}
			c, done := peer(t, data, fft, false)
			var err error
			if fft {
				_, _, err = c.ReadFFT(nil)
			} else {
				_, _, err = c.ReadIQ(make([]int16, 6))
			}
			if err == nil {
				t.Fatalf("accepted invalid frame %s %d", name, kind)
			}
			if kind < 3 || kind == 5 {
				if !errors.Is(err, ErrProtocol) {
					t.Fatal(err)
				}
			}
			if _, _, err := c.ReadIQ(nil); err == nil {
				t.Fatal("read after error")
			}
			if err := <-done; err != nil {
				t.Fatal(err)
			}
		}
	}
}

func TestRequestValidation(t *testing.T) {
	c := &Client{}
	if c.Request(1, 0, 0, ModeFFT) == nil {
		t.Fatal("FFT parameters accepted")
	}
	if c.Request(0, 0, 0, 2) == nil {
		t.Fatal("unknown mode accepted")
	}
}
