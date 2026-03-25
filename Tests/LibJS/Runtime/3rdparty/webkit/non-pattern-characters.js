test("non-pattern-characters", () => {
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

    description(
        "This page tests handling of characters which, according to ECMA 262, are not regular expression PatternCharacters. Those characters are: ^ $ \\ . * + ? ( ) [ ] { } |"
    );

    // We test two cases, to match the two cases in the WREC parser.
    // Test 1: the character stand-alone.
    // Test 2: the character following a PatternCharacter.

    var regexp;

    // ^: Always allowed, always an assertion.

    regexp = /^/g;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /\n^/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('\\n\\n')");
    shouldBe("regexp.lastIndex", "1");

    // $: Always allowed, always an assertion.

    regexp = /$/g;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /\n$/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('\\n\\n')");
    shouldBe("regexp.lastIndex", "1");

    // \: Only allowed as a prefix. Contrary to spec, always treated as an
    // IdentityEscape when followed by an invalid escape postfix.
    regexp = /\z/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('z')"); // invalid postfix => IdentityEscape

    regexp = /a\z/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('az')"); // invalid postfix => IdentityEscape

    regexp = /\_/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('_')"); // invalid postfix => IdentityEscape

    regexp = /a\_/;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a_')"); // invalid postfix => IdentityEscape

    debug("\nTesting regexp: " + "[invalid \\ variations]");
    shouldThrow("/\\/"); // no postfix => not allowed
    shouldThrow("/a\\/"); // no postfix => not allowed

    // .: Always allowed, always a non-newline wildcard.
    regexp = /./;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a')");
    shouldBeFalse("regexp.test('\\n')");

    regexp = /a./;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('aa')");
    shouldBeFalse("regexp.test('a\\n')");

    // *: Only allowed as a postfix to a PatternCharacter. Behaves as a {0,Infinity} quantifier.
    regexp = /a*/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('b')");
    shouldBe("regexp.lastIndex", "0");

    shouldBeTrue("regexp.test('aaba')");
    shouldBe("regexp.lastIndex", "2");

    debug("\nTesting regexp: " + "[invalid * variations]");
    shouldThrow("/*/"); // Unterminated comment.
    shouldThrow("/^*/"); // Prefixed by ^ to avoid confusion with comment syntax.

    // +: Only allowed as a postfix to a PatternCharacter. Behaves as a {1,Infinity} quantifier.
    regexp = /a+/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeFalse("regexp.test('b')");

    shouldBeTrue("regexp.test('aaba')");
    shouldBe("regexp.lastIndex", "2");

    debug("\nTesting regexp: " + "[invalid + variations]");
    shouldThrow("/+/");

    // ?: Only allowed as a postfix to a PatternCharacter. Behaves as a {0,1} quantifier.
    regexp = /a?/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('b')");
    shouldBe("regexp.lastIndex", "0");

    shouldBeTrue("regexp.test('aaba')");
    shouldBe("regexp.lastIndex", "1");

    debug("\nTesting regexp: " + "[invalid ? variations]");
    shouldThrow("/?/");

    // (: Only allowed if it matches a ).
    debug("\nTesting regexp: " + "[invalid ( variations]");
    shouldThrow("/(/");
    shouldThrow("/a(/");

    // ): Only allowed if it matches a (.
    debug("\nTesting regexp: " + "[invalid ) variations]");
    shouldThrow("/)/");
    shouldThrow("/a)/");

    // [: Only allowed if it matches a ] and the stuff in between is a valid character class.
    debug("\nTesting regexp: " + "[invalid [ variations]");
    shouldThrow("/[/");
    shouldThrow("/a[/");

    shouldThrow("/[b-a]/");
    shouldThrow("/a[b-a]/");

    // ]: Closes a ]. Contrary to spec, if no [ was seen, acts as a regular PatternCharacter.
    regexp = /]/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test(']')");
    shouldBe("regexp.lastIndex", "1");

    regexp = /a]/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a]')");
    shouldBe("regexp.lastIndex", "2");

    // {: Begins a quantifier. Contrary to spec, if no } is seen, or if the stuff in
    // between does not lex as a quantifier, acts as a regular PatternCharacter. If
    // the stuff in between does lex as a quantifier, but the quantifier is invalid,
    // acts as a syntax error.
    regexp = /{/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{')");
    shouldBe("regexp.lastIndex", "1");

    regexp = /a{/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a{')");
    shouldBe("regexp.lastIndex", "2");

    regexp = /{a/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{a')");
    shouldBe("regexp.lastIndex", "2");

    regexp = /a{a/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a{a')");
    shouldBe("regexp.lastIndex", "3");

    regexp = /{1,/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{1,')");
    shouldBe("regexp.lastIndex", "3");

    regexp = /a{1,/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a{1,')");
    shouldBe("regexp.lastIndex", "4");

    regexp = /{1,a/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{1,a')");
    shouldBe("regexp.lastIndex", "4");

    regexp = /{1,0/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{1,0')");
    shouldBe("regexp.lastIndex", "4");

    regexp = /{1, 0}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('{1, 0}')");
    shouldBe("regexp.lastIndex", "6");

    regexp = /a{1, 0}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a{1, 0}')");
    shouldBe("regexp.lastIndex", "7");

    try {
        regexp = new RegExp("a{1,0", "gm");
    } catch (e) {
        regexp = e;
    } // Work around exception thrown in Firefox -- probably too weird to be worth matching.
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a{1,0')");
    shouldBe("regexp.lastIndex", "5");

    regexp = /a{0}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a')");
    shouldBe("regexp.lastIndex", "0");
    shouldBeTrue("regexp.test('b')");
    shouldBe("regexp.lastIndex", "0");

    debug("\nTesting regexp: " + "[invalid {} variations]");
    shouldThrow("/{0}/");
    shouldThrow("/{1,0}/");
    shouldThrow("/a{1,0}/");

    // }: Ends a quantifier. Same rules as for {.
    regexp = /}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('}')");
    shouldBe("regexp.lastIndex", "1");

    regexp = /a}/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('a}')");
    shouldBe("regexp.lastIndex", "2");

    // |: Always allowed, always separates two alternatives.
    regexp = new RegExp("", "gm");
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /|/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('|')");
    shouldBe("regexp.lastIndex", "0");

    regexp = /a|/gm;
    debug("\nTesting regexp: " + regexp);
    shouldBeTrue("regexp.test('|')");
    shouldBe("regexp.lastIndex", "0");
});
