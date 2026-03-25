test.xfail("repeat-match-waldemar", () => {
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

    description(
        "Some test cases identified by Waldemar Horwat in response to this bug: https://bugs.webkit.org/show_bug.cgi?id=48101"
    );

    shouldBe('/(?:a*?){2,}/.exec("aa")', '["aa"]');
    shouldBe('/(?:a*?){2,}/.exec("a")', '["a"]');
    shouldBe('/(?:a*?){2,}/.exec("")', '[""]');

    shouldBe('/(?:a*?)/.exec("aa")', '[""]');
    shouldBe('/(?:a*?)/.exec("a")', '[""]');
    shouldBe('/(?:a*?)/.exec("")', '[""]');

    shouldBe('/(?:a*?)(?:a*?)(?:a*?)/.exec("aa")', '[""]');
    shouldBe('/(?:a*?)(?:a*?)(?:a*?)/.exec("a")', '[""]');
    shouldBe('/(?:a*?)(?:a*?)(?:a*?)/.exec("")', '[""]');

    shouldBe('/(?:a*?){2}/.exec("aa")', '[""]');
    shouldBe('/(?:a*?){2}/.exec("a")', '[""]');
    shouldBe('/(?:a*?){2}/.exec("")', '[""]');

    shouldBe('/(?:a*?){2,3}/.exec("aa")', '["a"]');
    shouldBe('/(?:a*?){2,3}/.exec("a")', '["a"]');
    shouldBe('/(?:a*?){2,3}/.exec("")', '[""]');

    shouldBe('/(?:a*?)?/.exec("aa")', '["a"]');
    shouldBe('/(?:a*?)?/.exec("a")', '["a"]');
    shouldBe('/(?:a*?)?/.exec("")', '[""]');

    shouldBe('/(?:a*?)*/.exec("aa")', '["aa"]');
    shouldBe('/(?:a*?)*/.exec("a")', '["a"]');
    shouldBe('/(?:a*?)*/.exec("")', '[""]');
});
