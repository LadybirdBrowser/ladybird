describe("basic functionality", () => {
    test("observes an inherited species constructor", () => {
        const originalSpecies = Object.getOwnPropertyDescriptor(RegExp, Symbol.species);
        try {
            Object.defineProperty(RegExp, Symbol.species, {
                configurable: true,
                value: function Species() {
                    throw new Error("BLOCKED");
                },
            });

            expect(() => "SAFE".split(/x/)).toThrowWithMessage(Error, "BLOCKED");
        } finally {
            Object.defineProperty(RegExp, Symbol.species, originalSpecies);
        }
    });
});
