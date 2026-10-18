// Receives transferred streams on its port, uses them, and reports the result.
class StreamProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.onmessage = async ({ data }) => {
      try {
        this.port.postMessage({ result: await this.use(data) });
      } catch (e) {
        this.port.postMessage({ error: `${e}` });
      }
    };
    this.port.onmessageerror = () => this.port.postMessage({ error: 'messageerror' });
  }

  async use({ kind, stream }) {
    if (kind === 'readable') {
      const { value } = await stream.getReader().read();
      return value;
    }
    if (kind === 'writable') {
      const writer = stream.getWriter();
      await writer.write('chunk from the worklet');
      await writer.close();
      return 'written';
    }
    const writer = stream.writable.getWriter();
    const reader = stream.readable.getReader();
    writer.write('chunk through the worklet');
    const { value } = await reader.read();
    return value;
  }

  process() {
    return true;
  }
}

registerProcessor('stream-processor', StreamProcessor);
