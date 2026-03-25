test("quantified-assertions", () => {
    // WebKit assertion compatibility shim for Ladybird's test-js harness

    function debug(msg) {}
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

    description("This page tests assertions followed by quantifiers.");

    var regexp;

    regexp = /(?=a){0}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /(?=a){1}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /(?!a){0}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('b')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /(?!a){1}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('b')");
    shouldBe("regexp.lastIndex", "0");

    shouldBeTrue('/^(?=a)?b$/.test("b")');
});
