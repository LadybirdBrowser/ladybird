// JIT code looks up the named properties of numbers and booleans in their
// prototypes inline, like those of strings. These tests run every function
// many times, so that with a low JIT threshold they are compiled.

const iterations = 100;

function getToString(value) {
    return value.toString;
}

function hex(value) {
    return value.toString(16);
}

function getFlag(value) {
    return value.flag;
}

describe("Property gets on primitives in JIT code", () => {
    test("methods of numbers and booleans", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(hex(i)).toBe(i.toString(16));
            expect(hex(i + 0.5)).toBe((i + 0.5).toString(16));
            expect(getToString(i)).toBe(Number.prototype.toString);
            expect(getToString(i % 2 === 0)).toBe(Boolean.prototype.toString);
            expect(getToString("string")).toBe(String.prototype.toString);
            expect(getToString({})).toBe(Object.prototype.toString);
        }
        expect(hex(-0)).toBe("0");
        expect(hex(NaN)).toBe("NaN");
        expect(hex(Infinity)).toBe("Infinity");
        expect(() => getToString(undefined)).toThrow(TypeError);
        expect(() => getToString(null)).toThrow(TypeError);
        expect(getToString(Symbol())).toBe(Symbol.prototype.toString);
        expect(getToString(1n)).toBe(BigInt.prototype.toString);
    });

    test("prototype properties that change", () => {
        for (let i = 0; i < iterations; ++i) expect(getFlag(i)).toBeUndefined();
        Number.prototype.flag = "number";
        Boolean.prototype.flag = "boolean";
        try {
            for (let i = 0; i < iterations; ++i) {
                expect(getFlag(i)).toBe("number");
                expect(getFlag(i + 0.25)).toBe("number");
                expect(getFlag(i % 2 === 0)).toBe("boolean");
            }
            Number.prototype.flag = "changed";
            expect(getFlag(1)).toBe("changed");
            let receiver;
            Object.defineProperty(Number.prototype, "flag", {
                get() {
                    "use strict";
                    receiver = this;
                    return "getter";
                },
                configurable: true,
            });
            expect(getFlag(42)).toBe("getter");
            expect(receiver).toBe(42);
            expect(getFlag(true)).toBe("boolean");
        } finally {
            delete Number.prototype.flag;
            delete Boolean.prototype.flag;
        }
        expect(getFlag(1)).toBeUndefined();
        expect(getFlag(false)).toBeUndefined();
    });
});
