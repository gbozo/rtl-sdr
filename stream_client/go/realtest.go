package main

import (
	"fmt"
	"math"
	"os"
	"os/signal"
	"sort"
	"strconv"
	"time"

	"github.com/gbozo/rtl-sdr/stream_client/go/rtlstream"
)

func main() {
	host := "127.0.0.1"
	iqPort := 1234
	fftPort := 0

	if len(os.Args) >= 2 {
		iqPort, _ = strconv.Atoi(os.Args[1])
	}
	if len(os.Args) >= 3 {
		fftPort, _ = strconv.Atoi(os.Args[2])
	}

	interrupt := make(chan os.Signal, 1)
	signal.Notify(interrupt, os.Interrupt)

	go func() {
		c, err := rtlstream.Connect(host, iqPort)
		if err != nil {
			fmt.Fprintf(os.Stderr, "IQ connect: %v\n", err)
			return
		}
		defer c.Close()

		err = c.Request(100600000, 2400000, 200000, rtlstream.ModeIQ)
		if err != nil {
			fmt.Fprintf(os.Stderr, "IQ request: %v\n", err)
			return
		}
		fmt.Println("=== IQ client connected ===")

		samples := make([]int16, 4096)
		var maxRMS float64

		for i := 0; ; i++ {
			select {
			case <-interrupt:
				return
			default:
			}

			hdr, n, err := c.ReadIQ(samples)
			if err != nil {
				fmt.Fprintf(os.Stderr, "IQ read: %v\n", err)
				return
			}

			var meanI, meanQ, meanP float64
			npairs := n / 2
			for j := 0; j < npairs; j++ {
				I, Q := float64(samples[j*2]), float64(samples[j*2+1])
				meanI += I
				meanQ += Q
				meanP += I*I + Q*Q
			}
			meanI /= float64(npairs)
			meanQ /= float64(npairs)
			meanP /= float64(npairs)
			rms := math.Sqrt(meanP)
			if rms > maxRMS {
				maxRMS = rms
			}
			if i < 5 || i%50 == 49 {
			fmt.Printf("IQ #%d: freq=%d rate=%d seq=%d DC=(%.0f,%.0f) RMS=%.0f maxRMS=%.0f\n",
					i, hdr.Freq, hdr.Rate, hdr.Seq,
					meanI, meanQ, rms, maxRMS)
			}
		}
	}()

	if fftPort > 0 {
		time.Sleep(500 * time.Millisecond)
		go func() {
			c, err := rtlstream.Connect(host, fftPort)
			if err != nil {
				fmt.Fprintf(os.Stderr, "FFT connect: %v\n", err)
				return
			}
			defer c.Close()

			err = c.Request(100600000, 2400000, 200000, rtlstream.ModeFFT)
			if err != nil {
				fmt.Fprintf(os.Stderr, "FFT request: %v\n", err)
				return
			}
			fmt.Println("=== FFT client connected ===")

			var power []float32
			for i := 0; i < 200; i++ {
				select {
				case <-interrupt:
					return
				default:
				}

				hdr, power, err := c.ReadFFT(power)
				if err != nil {
					fmt.Fprintf(os.Stderr, "FFT read: %v\n", err)
					return
				}

				if i == 0 {
					fmt.Printf("FFT: %d bins, %d Hz/line\n",
						hdr.Bins, hdr.Rate/uint64(hdr.Bins))
				}

				p := make([]float64, hdr.Bins)
				var s, mx float64
				for j := range p {
					p[j] = float64(power[j])
					s += p[j]
					if p[j] > mx {
						mx = p[j]
					}
				}
				avg := s / float64(hdr.Bins)
				sort.Float64s(p)
				t5 := p[len(p)-5:]
				if i < 3 || i%25 == 24 {
					fmt.Printf("FFT #%d: max=%.0f avg=%.0f top5=%v\n",
						i, mx, avg, t5)
				}
			}
			fmt.Println("FFT done (200 frames)")
		}()
	}

	select {
	case <-interrupt:
		fmt.Println("\nInterrupted")
	case <-time.After(12 * time.Second):
	}
}
