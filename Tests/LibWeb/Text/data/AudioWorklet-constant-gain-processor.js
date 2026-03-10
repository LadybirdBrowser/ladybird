registerProcessor(
    "constant-gain",
    class extends AudioWorkletProcessor {
        static get parameterDescriptors() {
            return [
                {
                    name: "gain",
                    defaultValue: 0.0,
                    minValue: 0.0,
                    maxValue: 1.0,
                },
            ];
        }

        process(inputs, outputs, parameters) {
            let out = outputs[0][0];
            let gain = parameters.gain[0];

            for (let i = 0; i < out.length; ++i) out[i] = gain;

            return true;
        }
    }
);
