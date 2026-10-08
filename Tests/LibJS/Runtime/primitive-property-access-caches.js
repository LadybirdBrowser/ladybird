// Indices of strings and named properties of primitives have fast paths in the interpreter. These tests run each
// access site many times, so that the fast paths and their caches are used, and then change what they find.

const iterations = 50;

describe("indices of strings", () => {
    test("ASCII and other code units, and indices out of range", () => {
        function at(string, index) {
            return string[index];
        }
        const string = "abé\u{1F600}";
        for (let i = 0; i < iterations; ++i) {
            expect(at(string, 0)).toBe("a");
            expect(at(string, 1)).toBe("b");
            expect(at(string, 2)).toBe("é");
            expect(at(string, 3)).toBe("\ud83d");
            expect(at(string, 4)).toBe("\ude00");
            expect(at(string, 5)).toBeUndefined();
            expect(at(string, -1)).toBeUndefined();
            expect(at("x".repeat(100) + "y", 100)).toBe("y");
        }
    });

    test("indices beyond the string find the properties of String.prototype", () => {
        function at(string, index) {
            return string[index];
        }
        for (let i = 0; i < iterations; ++i) expect(at("abc", 5)).toBeUndefined();
        String.prototype[5] = "five";
        try {
            expect(at("abc", 5)).toBe("five");
            expect(at("abc", 1)).toBe("b");
        } finally {
            delete String.prototype[5];
        }
    });
});

describe("named properties of primitives", () => {
    test("methods of the prototype of each kind", () => {
        function method(value) {
            return value.toString;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(method("a")).toBe(String.prototype.toString);
            expect(method(1)).toBe(Number.prototype.toString);
            expect(method(true)).toBe(Boolean.prototype.toString);
            expect(method(1n)).toBe(BigInt.prototype.toString);
            expect(method(Symbol.iterator)).toBe(Symbol.prototype.toString);
        }
    });

    test("changes to the prototype are seen", () => {
        function get(value) {
            return value.shout;
        }
        for (let i = 0; i < iterations; ++i) expect(get("a")).toBeUndefined();
        String.prototype.shout = 1;
        try {
            for (let i = 0; i < iterations; ++i) expect(get("a")).toBe(1);
            Object.defineProperty(String.prototype, "shout", {
                get() {
                    "use strict";
                    return typeof this;
                },
                configurable: true,
            });
            expect(get("a")).toBe("string");
        } finally {
            delete String.prototype.shout;
        }
        expect(get("a")).toBeUndefined();
    });

    test("nullish bases still throw", () => {
        function get(value) {
            return value.foo;
        }
        for (let i = 0; i < iterations; ++i) expect(get("a")).toBeUndefined();
        expect(() => get(null)).toThrow(TypeError);
        expect(() => get(undefined)).toThrow(TypeError);
    });
});
