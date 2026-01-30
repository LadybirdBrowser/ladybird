class PassthruProcessor extends AudioWorkletProcessor {
    process(inputs, outputs) {
        const input = inputs[0] || [];
        const output = outputs[0] || [];

        for (let ch = 0; ch < output.length; ch++) {
            const inChannel = input[ch];
            const outChannel = output[ch];
            if (!outChannel) continue;
            if (inChannel) outChannel.set(inChannel);
            else outChannel.fill(0);
        }

        return true;
    }
}

registerProcessor("passthru", PassthruProcessor);
