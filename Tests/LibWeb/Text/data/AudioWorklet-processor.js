registerProcessor(
    "dummy",
    class {
        static get parameterDescriptors() {
            return [
                {
                    name: "gain",
                    defaultValue: 0.5,
                    minValue: 0,
                    maxValue: 1,
                    automationRate: "a-rate",
                },
                {
                    name: "detune",
                    defaultValue: 0,
                    minValue: -1200,
                    maxValue: 1200,
                    automationRate: "k-rate",
                },
            ];
        }
    }
);
