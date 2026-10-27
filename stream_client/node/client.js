const net = require('net');

const MAGIC_REQ = 0x52543252;
const MAGIC_IQ = 0x52543249;
const MAGIC_FFT = 0x52543246;
const MAGIC_EVT = 0x52543245;
const MODE_IQ = 0;
const MODE_FFT = 1;
const EVT_FREQ_CHANGE = 1;
const EVT_STREAM_END = 2;
const MAX_IQ_COMPONENTS = 262144;
const MAX_FFT_BINS = 65536;

function uint64(value) {
  if (typeof value !== 'bigint' && !Number.isSafeInteger(value)) {
    throw new TypeError('uint64 requires a BigInt or safe integer');
  }
  const result = BigInt(value);
  if (result < 0n || result > 0xffffffffffffffffn) {
    throw new RangeError('uint64 out of range');
  }
  return result;
}

class StreamEvent {
  constructor(type, freq = 0n) {
    this.type = type;
    this.freq = freq;
  }
}

class RtlsdrStreamClient {
  constructor() {
    this.socket = null;
    this._reading = false;
    this._pending = null;
    this._terminal = null;
    this._requested = false;
  }

  connect(host, port) {
    if (this.socket || this._reading) return Promise.reject(new Error('Already connected or reading'));
    this._terminal = null;
    this._requested = false;
    const socket = this.socket = new net.Socket();
    socket.on('readable', () => this._pump());
    socket.on('end', () => this._finish(new Error('Unexpected EOF')));
    socket.on('error', (err) => this._finish(err));
    socket.on('close', () => this._finish(new Error('Socket closed')));
    return new Promise((resolve, reject) => {
      const failed = (err) => { cleanup(); reject(err); };
      const closed = () => failed(new Error('Socket closed before connection'));
      const cleanup = () => {
        socket.removeListener('error', failed);
        socket.removeListener('close', closed);
      };
      socket.once('error', failed);
      socket.once('close', closed);
      socket.connect(port, host, () => { cleanup(); resolve(); });
    });
  }

  close() {
    this._finish(new Error('Client closed'));
    if (this.socket) this.socket.destroy();
  }

  _finish(err) {
    if (!this._terminal) this._terminal = err;
    this._pump();
  }

  async request(freq, rate, bandwidth, mode) {
    freq = uint64(freq);
    rate = uint64(rate);
    bandwidth = uint64(bandwidth);
    if (this._requested || (mode !== MODE_IQ && mode !== MODE_FFT) ||
        (mode === MODE_FFT && (freq !== 0n || rate !== 0n || bandwidth !== 0n))) {
      throw new Error('Invalid or repeated request');
    }
    const buf = Buffer.alloc(29);
    buf.writeUInt32BE(MAGIC_REQ, 0);
    buf.writeBigUInt64BE(freq, 4);
    buf.writeBigUInt64BE(rate, 12);
    buf.writeBigUInt64BE(bandwidth, 20);
    buf[28] = mode;
    this._requested = true;
    await this._send(buf);
  }

  readIQ() { return this._readFrame(MAGIC_IQ); }
  readFFT() { return this._readFrame(MAGIC_FFT); }

  async _readFrame(expected) {
    if (this._reading) throw new Error('Concurrent frame reads are not allowed');
    this._reading = true;
    try {
      const magic = (await this._recv(4)).readUInt32BE(0);
      if (magic === MAGIC_EVT) {
        const type = (await this._recv(4)).readUInt32BE(0);
        if (type === EVT_FREQ_CHANGE) {
          return { event: new StreamEvent(type, (await this._recv(8)).readBigUInt64BE(0)) };
        }
        if (type === EVT_STREAM_END) {
          this.close();
          return { event: new StreamEvent(type) };
        }
        throw new Error('Unknown event type');
      }
      if (magic !== expected) throw new Error('Bad RTLSTREAM/2 frame magic');
      const hbuf = await this._recv(28);
      const hdr = {
        magic,
        freq: hbuf.readBigUInt64BE(0),
        rate: hbuf.readBigUInt64BE(8),
        seq: hbuf.readBigUInt64BE(16),
      };
      const count = hbuf.readUInt32BE(24);
      if (magic === MAGIC_IQ) {
        if (count > MAX_IQ_COMPONENTS || count % 2 !== 0) throw new Error('Invalid I/Q component count');
        hdr.nsamples = count;
        const data = await this._recv(count * 2);
        const samples = new Int16Array(count);
        for (let i = 0; i < count; i++) samples[i] = data.readInt16BE(i * 2);
        return { hdr, samples };
      }
      if (count < 2 || count > MAX_FFT_BINS) throw new Error('Invalid FFT bin count');
      hdr.bins = count;
      const data = await this._recv(count * 4);
      const power = new Float32Array(count);
      for (let i = 0; i < count; i++) power[i] = data.readFloatBE(i * 4);
      return { hdr, power };
    } catch (err) {
      this.close();
      throw err;
    } finally {
      this._reading = false;
    }
  }

  _send(buf) {
    return new Promise((resolve, reject) => {
      const socket = this.socket;
      if (!socket || this._terminal || socket.destroyed) {
        reject(this._terminal || new Error('Not connected'));
        return;
      }
      const cleanup = () => {
        socket.removeListener('error', failed);
        socket.removeListener('close', closed);
      };
      const failed = (err) => { cleanup(); reject(err); };
      const closed = () => failed(new Error('Socket closed during request'));
      socket.once('error', failed);
      socket.once('close', closed);
      socket.write(buf, (err) => {
        cleanup();
        if (err) { this.close(); reject(err); }
        else resolve();
      });
    });
  }

  _recv(len) {
    if (len === 0) return Promise.resolve(Buffer.alloc(0));
    return new Promise((resolve, reject) => {
      this._pending = { len, resolve, reject };
      this._pump();
    });
  }

  _pump() {
    const pending = this._pending;
    if (!pending) return;
    const socket = this.socket;
    if (this._terminal || !socket) {
      this._pending = null;
      pending.reject(this._terminal || new Error('Not connected'));
      return;
    }
    const data = socket && socket.read(pending.len);
    if (data !== null && data !== undefined) {
      this._pending = null;
      if (data.length === pending.len) pending.resolve(data);
      else pending.reject(new Error('Truncated frame at EOF'));
    }
  }
}

module.exports = { RtlsdrStreamClient, StreamEvent, MAGIC_REQ, MAGIC_IQ, MAGIC_FFT,
  MAGIC_EVT, MODE_IQ, MODE_FFT, EVT_FREQ_CHANGE, EVT_STREAM_END,
  MAX_IQ_COMPONENTS, MAX_FFT_BINS };
