registerProcessor(
    "bad-default",
    class {
        static get parameterDescriptors() {
            return [{ name: "gain", defaultValue: 2, minValue: 0, maxValue: 1 }];
        }
    }
);
