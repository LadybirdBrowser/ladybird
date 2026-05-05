registerProcessor(
    "one-shot",
    class extends AudioWorkletProcessor {
        constructor(options) {
            super(options);
            this._called = false;
        }

        process(inputs, outputs, parameters) {
            const out = outputs[0][0];
            if (!this._called) {
                for (let i = 0; i < out.length; ++i) out[i] = 1.0;
                this._called = true;
                return false;
            }

            for (let i = 0; i < out.length; ++i) out[i] = 0.5;
            return true;
        }
    }
);
