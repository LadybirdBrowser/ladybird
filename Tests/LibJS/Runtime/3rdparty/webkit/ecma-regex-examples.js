test("ecma-regex-examples", () => {
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

    description("This page tests the regex examples from the ECMA-262 specification.");

    var regex01 = /a|ab/;
    shouldBe('regex01.exec("abc")', '["a"]');

    var regex02 = /((a)|(ab))((c)|(bc))/;
    shouldBe('regex02.exec("abc")', '["abc", "a", "a", undefined, "bc", undefined, "bc"]');

    var regex03 = /a[a-z]{2,4}/;
    shouldBe('regex03.exec("abcdefghi")', '["abcde"]');

    var regex04 = /a[a-z]{2,4}?/;
    shouldBe('regex04.exec("abcdefghi")', '["abc"]');

    var regex05 = /(aa|aabaac|ba|b|c)*/;
    shouldBe('regex05.exec("aabaac")', '["aaba", "ba"]');

    var regex06 = /^(a+)\1*,\1+$/;
    shouldBe('"aaaaaaaaaa,aaaaaaaaaaaaaaa".replace(regex06,"$1")', '"aaaaa"');

    var regex07 = /(z)((a+)?(b+)?(c))*/;
    shouldBe('regex07.exec("zaacbbbcac")', '["zaacbbbcac", "z", "ac", "a", undefined, "c"]');

    var regex08 = /(a*)*/;
    shouldBe('regex08.exec("b")', '["", undefined]');

    var regex09 = /(a*)b\1+/;
    shouldBe('regex09.exec("baaaac")', '["b", ""]');

    var regex10 = /(?=(a+))/;
    shouldBe('regex10.exec("baaabac")', '["", "aaa"]');

    var regex11 = /(?=(a+))a*b\1/;
    shouldBe('regex11.exec("baaabac")', '["aba", "a"]');

    var regex12 = /(.*?)a(?!(a+)b\2c)\2(.*)/;
    shouldBe('regex12.exec("baaabaac")', '["baaabaac", "ba", undefined, "abaac"]');
});
