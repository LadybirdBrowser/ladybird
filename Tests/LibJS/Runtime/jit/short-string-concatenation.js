// JIT code finds the results of short string concatenations in the VM's
// cache of strings, and the strings of small integers it concatenates in the
// VM's cache of those. These tests run every function many times, so that with
// a low JIT threshold they are compiled.

const iterations = 100;

function concatenate(a, b) {
    return a + b;
}

describe("Short string concatenation in JIT code", () => {
    test("results up to and beyond the short string length", () => {
        const pieces = ["a", "bc", "def", "ghij", "klmno", "pqrstu", "vwxyz12"];
        for (let i = 0; i < iterations; ++i) {
            for (const a of pieces) {
                for (const b of pieces) {
                    const result = concatenate(a, b);
                    expect(result).toBe(a + b);
                    expect(result.length).toBe(a.length + b.length);
                    expect(result.charCodeAt(a.length)).toBe(b.charCodeAt(0));
                }
            }
        }
    });

    test("strings beyond ASCII, empty strings and other values", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(concatenate("é", "t")).toBe("ét");
            expect(concatenate("t", "é")).toBe("té");
            expect(concatenate("", "ab")).toBe("ab");
            expect(concatenate("ab", "")).toBe("ab");
            expect(concatenate("on", "click")).toBe("onclick");
            expect(concatenate("x", i % 10)).toBe("x" + (i % 10));
            expect(concatenate(i % 10, "_d")).toBe((i % 10) + "_d");
            expect(concatenate("a", null)).toBe("anull");
        }
    });

    test("results used as property keys and compared", () => {
        const object = { onclick: 1, onfocus: 2 };
        for (let i = 0; i < iterations; ++i) {
            const type = i % 2 ? "click" : "focus";
            expect(object[concatenate("on", type)]).toBe(i % 2 ? 1 : 2);
            expect(concatenate("on", type) === "on" + type).toBeTrue();
        }
    });

    test("integers and strings", () => {
        for (let i = 0; i < iterations * 20; ++i) {
            const n = i % 2000;
            expect(concatenate(n, "_d")).toBe(String(n) + "_d");
            expect(concatenate("item", n)).toBe("item" + String(n));
            expect(concatenate("", n)).toBe(String(n));
            expect(concatenate(n, "")).toBe(String(n));
            expect(concatenate(-n, "x")).toBe(String(-n) + "x");
            expect(concatenate(n, n)).toBe(2 * n);
            expect(concatenate(n + 0.5, "x")).toBe(String(n + 0.5) + "x");
        }
        expect(concatenate(2147483647, 1)).toBe(2147483648);
        expect(concatenate("x", 2147483647)).toBe("x2147483647");
    });
});
