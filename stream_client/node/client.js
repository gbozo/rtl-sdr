const net = require('net');

const MAGIC_REQ = 0x52545352;
const MAGIC_IQ  = 0x52545349;
const MAGIC_FFT = 0x52545346;
const MAGIC_EVT = 0x52545345;

const MODE_IQ  = 0;
const MODE_FFT = 1;

class RtlsdrStreamClient {
  constructor() {
    this.socket = null;
  }

  connect(host, port) {
    return new Promise((resolve, reject) => {
      this.socket = new net.Socket();
      this.socket.once('error', reject);
      this.socket.connect(port, host, () => {
        this.socket.removeListener('error', reject);
        resolve();
      });
    });
  }

  close() {
    if (this.socket) {
      this.socket.destroy();
      this.socket = null;
    }
  }

  async request(freq, rate, bandwidth, mode) {
    const buf = Buffer.alloc(29);
    buf.writeUInt32BE(MAGIC_REQ, 0);
    buf.writeBigUInt64BE(BigInt(freq), 4);
    buf.writeBigUInt64BE(BigInt(rate), 12);
    buf.writeBigUInt64BE(BigInt(bandwidth), 20);
    buf[28] = mode;
    await this._send(buf);
  }

  async readIQ(sampleCount = 4096) {
    const hbuf = await this._recv(32);
    const hdr = {
      magic: hbuf.readUInt32BE(0),
      freq:  Number(hbuf.readBigUInt64BE(4)),
      rate:  Number(hbuf.readBigUInt64BE(12)),
      seq:   Number(hbuf.readBigUInt64BE(20)),
      nsamples: hbuf.readUInt32BE(28),
    };

    if (hdr.magic !== MAGIC_IQ) {
      throw new Error(`Bad I/Q frame magic: 0x${hdr.magic.toString(16)}`);
    }

    const n = hdr.nsamples;
    const sbuf = await this._recv(n * 2);
    const samples = new Int16Array(n);
    for (let i = 0; i < n; i++) {
      samples[i] = sbuf.readInt16BE(i * 2);
    }

    return { hdr, samples };
  }

  async readFFT(binCount) {
    const hbuf = await this._recv(32);
    const hdr = {
      magic: hbuf.readUInt32BE(0),
      freq:  Number(hbuf.readBigUInt64BE(4)),
      rate:  Number(hbuf.readBigUInt64BE(12)),
      seq:   Number(hbuf.readBigUInt64BE(20)),
      bins:  hbuf.readUInt32BE(28),
    };

    if (hdr.magic !== MAGIC_FFT) {
      throw new Error(`Bad FFT frame magic: 0x${hdr.magic.toString(16)}`);
    }

    const n = binCount || hdr.bins;
    const fbuf = await this._recv(n * 4);
    const power = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      power[i] = fbuf.readFloatBE(i * 4);
    }

    return { hdr, power };
  }

  _send(buf) {
    return new Promise((resolve, reject) => {
      this.socket.write(buf, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
  }

  _recv(len) {
    return new Promise((resolve, reject) => {
      const buf = Buffer.alloc(len);
      let offset = 0;

      const onData = (chunk) => {
        chunk.copy(buf, offset);
        offset += chunk.length;
        if (offset >= len) {
          this.socket.removeListener('data', onData);
          this.socket.removeListener('error', onError);
          resolve(buf);
        }
      };

      const onError = (err) => {
        this.socket.removeListener('data', onData);
        this.socket.removeListener('error', onError);
        reject(err);
      };

      this.socket.on('data', onData);
      this.socket.on('error', onError);
    });
  }
}

module.exports = { RtlsdrStreamClient, MAGIC_REQ, MAGIC_IQ, MAGIC_FFT,
                   MAGIC_EVT, MODE_IQ, MODE_FFT };
