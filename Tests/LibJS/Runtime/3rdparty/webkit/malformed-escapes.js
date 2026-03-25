test("malformed-escapes", () => {
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

    description("This page tests handling of malformed escape sequences.");

    var regexp;

    regexp = /\ug/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('ug')");
    shouldBe("regexp.lastIndex", "2");

    regexp = /\xg/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('xg')");
    shouldBe("regexp.lastIndex", "2");

    regexp = /\c_/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('\\\\c_')");
    shouldBe("regexp.lastIndex", "3");

    regexp = /[\B]/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('B')");
    shouldBe("regexp.lastIndex", "1");

    regexp = /[\b]/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('\\b')");
    shouldBe("regexp.lastIndex", "1");

    regexp = /\8/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('\\\\8')");
    shouldBe("regexp.lastIndex", "2");

    regexp = /^[\c]$/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('c')");

    regexp = /^[\c_]$/;
    debug("\nTesting regexp: " + regexp);
    shouldBeFalse("regexp.test('c')");

    regexp = /^[\c]]$/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('c]')");
});
