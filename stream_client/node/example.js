const { RtlsdrStreamClient, MODE_IQ } = require('./client');

if (process.argv.length < 5) {
  console.error(`Usage: node ${process.argv[1]} <host> <port> <freq_hz>`);
  process.exit(1);
}

const host = process.argv[2];
const port = parseInt(process.argv[3]);
const freq = parseInt(process.argv[4]);

async function main() {
  const client = new RtlsdrStreamClient();

  try {
    await client.connect(host, port);
    console.log(`Connected to rtl_stream at ${host}:${port}, freq=${freq} Hz`);

    await client.request(freq, 2400000, 200000, MODE_IQ);
    console.log('Receiving I/Q frames (Ctrl+C to stop)...');

    let frameCount = 0;

    while (true) {
      const { hdr, samples } = await client.readIQ(2048);
      console.log(`I/Q frame: freq=${hdr.freq} rate=${hdr.rate} seq=${hdr.seq} sample[0]=(${samples[0]},${samples[1]})`);
      frameCount++;
    }
  } catch (err) {
    if (err.code !== 'ECONNRESET' && !err.message.includes('write after end')) {
      console.error('Error:', err.message);
    }
  } finally {
    client.close();
    console.log('\nDisconnected.');
  }
}

process.on('SIGINT', () => process.exit(0));
process.on('SIGTERM', () => process.exit(0));

main();
