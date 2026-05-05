class PortRoundtripProcessor extends AudioWorkletProcessor {
    constructor(nodeOptions) {
        super();

        this.port.onmessage = event => {
            if (event.data === "ping") this.port.postMessage("pong");
        };

        this.port.postMessage(nodeOptions);
    }

    process() {
        return true;
    }
}

registerProcessor("port-roundtrip-processor", PortRoundtripProcessor);
