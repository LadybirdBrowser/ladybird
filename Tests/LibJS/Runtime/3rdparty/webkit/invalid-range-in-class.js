test("invalid-range-in-class", () => {
    // WebKit assertion compatibility shim for Ladybird's test-js harness

    function description(msg) {
        // No-op, just used for test documentation in WebKit.
    }

    function shouldBe(actual_code, expected_code) {
        let actual = eval(actual_code);
        let expected = eval(expected_code);
        if (typeof actual === "string" && typeof expected === "string") {
            expect(actual).toBe(expected);
        } else if (Array.isArray(actual) && Array.isArray(expected)) {
            expect(actual).toEqual(expected);
        } else if (actual !== null && typeof actual === "object" && expected !== null && typeof expected === "object") {
            expect(actual).toEqual(expected);
        } else {
            expect(actual).toBe(expected);
        }
    }

    function shouldBeTrue(code) {
        expect(eval(code)).toBeTrue();
    }

    function shouldBeFalse(code) {
        expect(eval(code)).toBeFalse();
    }

    function shouldBeNull(code) {
        expect(eval(code)).toBeNull();
    }

    function shouldBeUndefined(code) {
        expect(eval(code)).toBeUndefined();
    }

    function shouldThrow(code, expected_error) {
        expect(() => eval(code)).toThrow();
    }

    function shouldNotThrow(code) {
        eval(code);
    }

    description("This page tests invalid character ranges in character classes.");

    // These test a basic range / non range.
    shouldBe('/[a-c]+/.exec("-acbd");', '["acb"]');
    shouldBe('/[a\\-c]+/.exec("-acbd")', '["-ac"]');

    // A reverse-range is invalid.
    shouldThrow('/[c-a]+/.exec("-acbd");');

    // A character-class in a range is invalid, according to ECMA-262, but we allow it.
    shouldBe('/[\\d-x]+/.exec("1-3xy");', '["1-3x"]');
    shouldBe('/[x-\\d]+/.exec("1-3xy");', '["1-3x"]');
    shouldBe('/[\\d-\\d]+/.exec("1-3xy");', '["1-3"]');

    // Whilst we break with ECMA-262's definition of CharacterRange, we do comply with
    // the grammar, and as such in the following regex a-z cannot be matched as a range.
    shouldBe('/[\\d-a-z]+/.exec("az1-3y");', '["az1-3"]');

    // An escaped hypen should not be confused for an invalid range.
    shouldBe('/[\\d\\-x]+/.exec("1-3xy");', '["1-3x"]');
    shouldBe('/[x\\-\\d]+/.exec("1-3xy");', '["1-3x"]');
    shouldBe('/[\\d\\-\\d]+/.exec("1-3xy");', '["1-3"]');

    // A hyphen after a character-class is not invalid.
    shouldBe('/[\\d-]+/.exec("1-3xy")', '["1-3"]');
});
