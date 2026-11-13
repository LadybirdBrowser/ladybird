// The text of a large source that nothing read for a while is compressed, and decompressed when something reads it
// again. parkSourceTexts() compresses every text that is large enough, and code must behave the same afterwards.

// NB: Only large sources are parked, so the code under test comes from an eval of a long string.
const padding = "/* padding */\n".repeat(2000);

function evalLargeSource(code) {
    return (0, eval)(`${padding}${code}\n${padding}`);
}

describe("parked source texts", () => {
    test("a function compiles for the first time from a parked source", () => {
        const outer = evalLargeSource("(function outer() { return function inner(a, b) { return a + b; }; })");
        expect(parkSourceTexts()).toBeGreaterThan(0);
        expect(outer()(1, 2)).toBe(3);
    });

    test("a function of a parked source has its source text", () => {
        const f = evalLargeSource("(function named(a, b) { return 'caf\u00e9 \u2019 \ud83d\ude00'; })");
        expect(parkSourceTexts()).toBeGreaterThan(0);
        expect(f.toString()).toBe("function named(a, b) { return 'caf\u00e9 \u2019 \ud83d\ude00'; }");
        expect(f()).toBe("caf\u00e9 \u2019 \ud83d\ude00");
    });

    test("an error thrown from a parked source reports its position", () => {
        const f = evalLargeSource("(function thrower() {\n    return undefinedName.property;\n})");
        expect(parkSourceTexts()).toBeGreaterThan(0);
        expect(f).toThrowWithMessage(ReferenceError, "'undefinedName' is not defined");
        expect(parkSourceTexts()).toBeGreaterThan(0);
        expect(f).toThrowWithMessage(ReferenceError, "'undefinedName' is not defined");
    });

    test("a text can be parked again after it was read", () => {
        const outer = evalLargeSource(
            "(function () { return [function a() { return 1; }, function b() { return 2; }]; })"
        );
        expect(parkSourceTexts()).toBeGreaterThan(0);
        const [a, b] = outer();
        expect(a()).toBe(1);
        expect(parkSourceTexts()).toBeGreaterThan(0);
        expect(b()).toBe(2);
        expect(b.toString()).toBe("function b() { return 2; }");
    });
});
