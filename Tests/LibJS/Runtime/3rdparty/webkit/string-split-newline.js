test("string-split-newline", () => {
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

    description("This test checks the SIMD-optimized newline splitting patterns for correctness.");

    // Test the optimized patterns: /\r\n?|\n/ and /\n|\r\n?/
    // These patterns match LF (\n), CR (\r), and CRLF (\r\n)

    var newlinePatterns = [/\r\n?|\n/, /\n|\r\n?/];
    var newlineTypes = ["\\n", "\\r", "\\r\\n"];

    newlinePatterns.forEach(function (pattern, patternIndex) {
        shouldBe(`"".split(newlinePatterns[${patternIndex}])`, '[""]');
        shouldBe(`"no newlines".split(newlinePatterns[${patternIndex}])`, '["no newlines"]');
        shouldBe(`"abc".split(newlinePatterns[${patternIndex}])`, '["abc"]');
        shouldBe(
            `"Line1\\r\\nLine2\\nLine3\\rLine4".split(newlinePatterns[${patternIndex}])`,
            '["Line1", "Line2", "Line3", "Line4"]'
        );
        newlineTypes.forEach(function (nl) {
            shouldBe(`"${nl}".split(newlinePatterns[${patternIndex}])`, '["", ""]');
            shouldBe(`"${nl}text".split(newlinePatterns[${patternIndex}])`, '["", "text"]');
            shouldBe(`"a${nl}b".split(newlinePatterns[${patternIndex}])`, '["a", "b"]');
            shouldBe(`"a${nl}b${nl}c".split(newlinePatterns[${patternIndex}])`, '["a", "b", "c"]');
            shouldBe(`"text${nl}".split(newlinePatterns[${patternIndex}])`, '["text", ""]');
            shouldBe(`"${nl}${nl}".split(newlinePatterns[${patternIndex}])`, '["", "", ""]');
            shouldBe(`"a${nl}${nl}b".split(newlinePatterns[${patternIndex}])`, '["a", "", "b"]');
            shouldBe(`"a${nl}b${nl}c".split(newlinePatterns[${patternIndex}], 2)`, '["a", "b"]');
            shouldBe(`"a${nl}b${nl}".split(newlinePatterns[${patternIndex}], 2)`, '["a", "b"]');
            shouldBe(`"a${nl}b${nl}c${nl}".split(newlinePatterns[${patternIndex}], 3)`, '["a", "b", "c"]');
            shouldBe(
                `"This is a very long string that exceeds 32 chars to test vectorMatch in the SIMD-optimized function${nl}Second line".split(newlinePatterns[${patternIndex}])`,
                '["This is a very long string that exceeds 32 chars to test vectorMatch in the SIMD-optimized function", "Second line"]'
            );
        });
    });

    /(test)(123)/.exec("test123");
    shouldBe("RegExp.$1", '"test"');
    shouldBe("RegExp.$2", '"123"');

    "".split(/\r\n?|\n/);
    shouldBe("RegExp.$1", '"test"');
    shouldBe("RegExp.$2", '"123"');

    "a\nb\nc".split(/\r\n?|\n/);
    shouldBe("RegExp.$1", '""');
    shouldBe("RegExp.$2", '""');

    /(foo)(bar)/.exec("foobar");
    shouldBe("RegExp.$1", '"foo"');
    shouldBe("RegExp.$2", '"bar"');

    "x\ny\nz".split(/\n|\r\n?/);
    shouldBe("RegExp.$1", '""');
    shouldBe("RegExp.$2", '""');

    "hello\nworld".split(/\r\n?|\n/);
    shouldBe("RegExp.lastMatch", '"\\n"');
    "hello\r\nworld".split(/\r\n?|\n/);
    shouldBe("RegExp.lastMatch", '"\\r\\n"');
    "hello\rworld".split(/\r\n?|\n/);
    shouldBe("RegExp.lastMatch", '"\\r"');
});
