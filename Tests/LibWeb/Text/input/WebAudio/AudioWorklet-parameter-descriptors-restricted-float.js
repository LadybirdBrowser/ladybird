function reportRegisterProcessorResult(name, descriptor) {
    try {
        registerProcessor(
            name,
            class extends AudioWorkletProcessor {
                static get parameterDescriptors() {
                    return [descriptor];
                }

                process() {
                    return false;
                }
            }
        );
        globalThis.port.postMessage(`${name}: OK`);
    } catch (error) {
        globalThis.port.postMessage(`${name}: ${error.name}`);
    }
}

reportRegisterProcessorResult("default-infinity", { name: "param", defaultValue: Infinity });
reportRegisterProcessorResult("min-negative-infinity", { name: "param", minValue: -Infinity });
reportRegisterProcessorResult("max-infinity", { name: "param", maxValue: Infinity });
reportRegisterProcessorResult("nan", { name: "param", defaultValue: NaN });
reportRegisterProcessorResult("finite-out-of-range", { name: "param", defaultValue: 2, minValue: 0, maxValue: 1 });
