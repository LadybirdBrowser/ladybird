registerProcessor(
    "dup",
    class {
        static get parameterDescriptors() {
            return [
                { name: "gain", defaultValue: 0.5, minValue: 0, maxValue: 1 },
                { name: "gain", defaultValue: 0.5, minValue: 0, maxValue: 1 },
            ];
        }
    }
);
