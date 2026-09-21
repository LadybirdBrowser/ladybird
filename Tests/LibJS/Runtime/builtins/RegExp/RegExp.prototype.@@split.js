describe("basic functionality", () => {
    test("observes individual flag overrides", () => {
        const regexp = /^safe$/;
        const input = {
            toString() {
                Object.defineProperty(regexp, "ignoreCase", { value: true });
                return "SAFE";
            },
        };

        const result = RegExp.prototype[Symbol.split].call(regexp, input);
        expect(JSON.stringify(result)).toBe('["",""]');
    });

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

    test("observes species on a replaced inherited constructor", () => {
        const originalConstructor = Object.getOwnPropertyDescriptor(RegExp.prototype, "constructor");
        try {
            const Constructor = function () {};
            Object.defineProperty(Constructor, Symbol.species, {
                get() {
                    throw new Error("BLOCKED");
                },
            });
            Object.defineProperty(RegExp.prototype, "constructor", {
                configurable: true,
                writable: true,
                value: Constructor,
            });

            expect(() => "SAFE".split(/x/)).toThrowWithMessage(Error, "BLOCKED");
        } finally {
            Object.defineProperty(RegExp.prototype, "constructor", originalConstructor);
        }
    });

    test("observes a replaced inherited flag accessor", () => {
        const flags = [
            "flags",
            "hasIndices",
            "global",
            "ignoreCase",
            "multiline",
            "dotAll",
            "unicode",
            "unicodeSets",
            "sticky",
        ];

        for (const flag of flags) {
            const original = Object.getOwnPropertyDescriptor(RegExp.prototype, flag);
            try {
                Object.defineProperty(RegExp.prototype, flag, {
                    configurable: true,
                    get() {
                        throw new Error("BLOCKED " + flag);
                    },
                });

                expect(() => "a,b".split(/,/)).toThrowWithMessage(Error, "BLOCKED " + flag);
            } finally {
                Object.defineProperty(RegExp.prototype, flag, original);
            }
        }
    });
});
