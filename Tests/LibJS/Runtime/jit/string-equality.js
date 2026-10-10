// JIT code compares strings by their lengths and, for short strings, their
// contents inline. These tests run every function many times, so that with
// a low JIT threshold they are compiled.

const iterations = 100;

function equals(a, b) {
    return a === b;
}

function looselyEquals(a, b) {
    return a == b;
}

function differs(a, b) {
    return a !== b;
}

function made(...parts) {
    return parts.join("");
}

describe("String equality in JIT code", () => {
    test("strings made at run time", () => {
        for (let i = 0; i < iterations; ++i) {
            const short = made("a", "b", String(i % 10));
            expect(equals(short, "ab" + (i % 10))).toBeTrue();
            expect(equals(short, "ab" + ((i + 1) % 10))).toBeFalse();
            expect(differs(short, "ab" + (i % 10))).toBeFalse();
            expect(looselyEquals(short, "ab" + (i % 10))).toBeTrue();
            expect(equals(short, "abc" + i)).toBeFalse();

            const long = made("a longer string ", String(i));
            expect(equals(long, "a longer string " + i)).toBeTrue();
            expect(equals(long, "a longer strinG " + i)).toBeFalse();
            expect(equals(long, "a longer string " + i + "!")).toBeFalse();

            const rope = "left part " + i + " right part";
            expect(equals(rope, made("left part ", String(i), " right part"))).toBeTrue();
            expect(equals(rope, "left part " + i + " right parT")).toBeFalse();
        }
    });

    test("strings beyond ASCII and empty strings", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(equals(made("é", "té"), "été")).toBeTrue();
            expect(equals(made("é", "té"), "eté")).toBeFalse();
            expect(equals(made("ab", "c"), made("a", "bc"))).toBeTrue();
            expect(equals(made(""), "")).toBeTrue();
            expect(equals(made(""), "x")).toBeFalse();
            expect(equals(made("x"), made("x"))).toBeTrue();
        }
    });

    test("strings and other values", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(equals("1", 1)).toBeFalse();
            expect(looselyEquals("1", 1)).toBeTrue();
            expect(equals(made("ab"), { toString: () => "ab" })).toBeFalse();
            expect(looselyEquals(made("ab"), { toString: () => "ab" })).toBeTrue();
            expect(equals(made("null"), null)).toBeFalse();
        }
    });
});
