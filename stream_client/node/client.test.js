const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const { RtlsdrStreamClient, StreamEvent, EVT_FREQ_CHANGE, EVT_STREAM_END,
  MODE_IQ, MODE_FFT, MAX_IQ_COMPONENTS, MAX_FFT_BINS } = require('./client');

const fixture = (name) => Buffer.from(fs.readFileSync(
  path.join(__dirname, '../../tests/fixtures', `${name}.hex`), 'utf8').replace(/\s/g, ''), 'hex');
const freq = 9007199254740993n;
const seq = 0x0102030405060708n;
const delay = () => new Promise((resolve) => setImmediate(resolve));

async function withPeer(data, fft, fragment, run) {
  let peer;
  let requestError;
  const server = net.createServer((socket) => {
    peer = socket;
    socket.on('error', () => {});
    let request = Buffer.alloc(0);
    socket.on('data', async (chunk) => {
      request = Buffer.concat([request, chunk]);
      if (request.length < 29) return;
      try { assert.deepEqual(request, fixture(fft ? 'request_fft' : 'request_iq')); }
      catch (err) { requestError = err; socket.destroy(); return; }
      socket.removeAllListeners('data');
      if (data === null) return;
      let offset = 0;
      if (fragment) {
        for (; offset < Math.min(35, data.length); offset++) {
          socket.write(data.subarray(offset, offset + 1));
          await delay();
        }
      }
      socket.end(data.subarray(offset));
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new RtlsdrStreamClient();
  try {
    await client.connect('127.0.0.1', server.address().port);
    await client.request(fft ? 0n : freq, fft ? 0 : 240000, fft ? 0 : 192000, fft ? MODE_FFT : MODE_IQ);
    await assert.rejects(client.request(0, 0, 0, MODE_FFT), /repeated/);
    await run(client, () => peer);
    if (requestError) throw requestError;
  } finally {
    client.close();
    if (peer) peer.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
}

async function main() {
  for (const fft of [false, true]) {
    const frame = fixture(fft ? 'fft' : 'iq');
    const data = Buffer.concat([frame, fixture('frequency'), frame, frame, fixture('end')]);
    await withPeer(data, fft, true, async (client) => {
      const read = () => fft ? client.readFFT() : client.readIQ();
      for (let i = 0; i < 5; i++) {
        const result = await read();
        if (i === 1 || i === 4) {
          assert(result.event instanceof StreamEvent);
          assert.equal(result.event.type, i === 1 ? EVT_FREQ_CHANGE : EVT_STREAM_END);
          assert.equal(result.event.freq, i === 1 ? freq : 0n);
        } else {
          assert.equal(result.hdr.freq, freq);
          assert.equal(result.hdr.rate, 2400000n);
          assert.equal(result.hdr.seq, seq);
          assert.deepEqual(Array.from(fft ? result.power : result.samples),
            fft ? [0, 1, -2.5, .5] : [-32768, -1, 0, 1, 256, 32767]);
        }
      }
      await assert.rejects(read());
    });

    for (let kind = 0; kind < 8; kind++) {
      let data = Buffer.from(frame);
      if (kind === 0) data[2] = 0x53;
      if (kind === 1) data.writeUInt32BE(0xffffffff, 28);
      if (kind === 2) data.writeUInt32BE(1, 28);
      if (kind === 3) data = data.subarray(0, 17);
      if (kind === 4) data = data.subarray(0, data.length - 1);
      if (kind === 5) { data = fixture('frequency'); data[7] = 99; }
      if (kind === 6) data = fixture('frequency').subarray(0, 15);
      if (kind === 7) data = Buffer.alloc(0);
      await withPeer(data, fft, false, async (client) => {
        await assert.rejects(fft ? client.readFFT() : client.readIQ());
        assert(client.socket.destroyed);
        await assert.rejects(client.readIQ());
      });
    }

    const maximum = fft ? MAX_FFT_BINS : MAX_IQ_COMPONENTS;
    const large = Buffer.concat([frame.subarray(0, 32), Buffer.alloc(maximum * (fft ? 4 : 2))]);
    large.writeUInt32BE(maximum, 28);
    await withPeer(large, fft, false, async (client) => {
      const result = await (fft ? client.readFFT() : client.readIQ());
      assert.equal((fft ? result.power : result.samples).length, maximum);
    });
  }

  await withPeer(null, false, false, async (client) => {
    const pending = client.readIQ();
    await assert.rejects(client.readFFT(), /Concurrent/);
    client.close();
    await assert.rejects(pending, /closed/);
  });
  await withPeer(fixture('iq'), false, true, async (client) => {
    const pending = client.readIQ();
    await assert.rejects(client.readIQ(), /Concurrent/);
    assert.deepEqual(Array.from((await pending).samples), [-32768, -1, 0, 1, 256, 32767]);
  });
  await withPeer(Buffer.concat([fixture('end'), fixture('iq')]), false, false, async (client) => {
    assert.equal((await client.readIQ()).event.type, EVT_STREAM_END);
    await assert.rejects(client.readIQ(), /closed/);
  });
  await withPeer(null, false, false, async (client, peer) => {
    const pending = client.readIQ();
    while (!peer()) await delay();
    peer().destroy();
    await assert.rejects(pending);
  });
  await withPeer(null, false, false, async (client) => {
    const pending = client.readIQ();
    client.socket.destroy(new Error('injected socket error'));
    await assert.rejects(pending, /injected/);
  });
  const disconnected = new RtlsdrStreamClient();
  await assert.rejects(disconnected.request(Number(freq), 0, 0, MODE_IQ), /safe integer/);
  await assert.rejects(disconnected.request(1n, 0, 0, MODE_FFT), /Invalid/);
  await assert.rejects(disconnected.request(0, 0, 0, 2), /Invalid/);
  console.log('Node client fixtures passed');
}

const timeout = setTimeout(() => { console.error('Node tests timed out'); process.exit(1); }, 15000);
main().then(() => clearTimeout(timeout), (err) => {
  clearTimeout(timeout);
  console.error(err);
  process.exitCode = 1;
});
