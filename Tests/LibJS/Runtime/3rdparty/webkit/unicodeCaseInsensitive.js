test("unicodeCaseInsensitive", () => {
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

    description("https://bugs.webkit.org/show_bug.cgi?id=82063");

    shouldBeTrue('/ΣΤΙΓΜΑΣ/i.test("στιγμας")');
    shouldBeTrue('/ΔΣΔ/i.test("δςδ")');
    shouldBeTrue('/ς/i.test("σ")');
    shouldBeTrue('/σ/i.test("ς")');

    // Simple case, has no canonical equivalents
    shouldBeTrue('/\u1f16/i.test("\u1f16")');

    // Test the sets of USC2 code points that have more than one canonically equivalent value.
    function ucs2CodePoint(x) {
        var s = x.toString(16);
        while (s.length < 4) s = 0 + s;
        return eval('"\\u' + s + '"');
    }
    function testSet(set) {
        for (i in set) {
            for (j in set) {
                shouldBeTrue("/" + ucs2CodePoint(set[i]) + '/i.test("' + ucs2CodePoint(set[j]) + '")');
                shouldBeTrue(
                    "/[" +
                        ucs2CodePoint(set[i] - 1) +
                        "-" +
                        ucs2CodePoint(set[i] + 1) +
                        ']/i.test("' +
                        ucs2CodePoint(set[j]) +
                        '")'
                );
            }
        }
    }
    testSet([0x01c4, 0x01c5, 0x01c6]);
    testSet([0x01c7, 0x01c8, 0x01c9]);
    testSet([0x01ca, 0x01cb, 0x01cc]);
    testSet([0x01f1, 0x01f2, 0x01f3]);
    testSet([0x0392, 0x03b2, 0x03d0]);
    testSet([0x0395, 0x03b5, 0x03f5]);
    testSet([0x0398, 0x03b8, 0x03d1]);
    testSet([0x0345, 0x0399, 0x03b9, 0x1fbe]);
    testSet([0x039a, 0x03ba, 0x03f0]);
    testSet([0x00b5, 0x039c, 0x03bc]);
    testSet([0x03a0, 0x03c0, 0x03d6]);
    testSet([0x03a1, 0x03c1, 0x03f1]);
    testSet([0x03a3, 0x03c2, 0x03c3]);
    testSet([0x03a6, 0x03c6, 0x03d5]);
    testSet([0x1e60, 0x1e61, 0x1e9b]);

    // Test a couple of lo/hi pairs
    shouldBeTrue('/\u03cf/i.test("\u03cf")');
    shouldBeTrue('/\u03d7/i.test("\u03cf")');
    shouldBeTrue('/\u03cf/i.test("\u03d7")');
    shouldBeTrue('/\u03d7/i.test("\u03d7")');
    shouldBeTrue('/\u1f11/i.test("\u1f11")');
    shouldBeTrue('/\u1f19/i.test("\u1f11")');
    shouldBeTrue('/\u1f11/i.test("\u1f19")');
    shouldBeTrue('/\u1f19/i.test("\u1f19")');

    // Test an aligned alternating capitalization pair.
    shouldBeFalse('/\u0489/i.test("\u048a")');
    shouldBeTrue('/\u048a/i.test("\u048a")');
    shouldBeTrue('/\u048b/i.test("\u048a")');
    shouldBeFalse('/\u048c/i.test("\u048a")');
    shouldBeFalse('/\u0489/i.test("\u048b")');
    shouldBeTrue('/\u048a/i.test("\u048b")');
    shouldBeTrue('/\u048b/i.test("\u048b")');
    shouldBeFalse('/\u048c/i.test("\u048b")');
    shouldBeTrue('/[\u0489-\u048a]/i.test("\u048b")');
    shouldBeTrue('/[\u048b-\u048c]/i.test("\u048a")');

    // Test an unaligned alternating capitalization pair.
    shouldBeFalse('/\u04c4/i.test("\u04c5")');
    shouldBeTrue('/\u04c5/i.test("\u04c5")');
    shouldBeTrue('/\u04c6/i.test("\u04c5")');
    shouldBeFalse('/\u04c7/i.test("\u04c5")');
    shouldBeFalse('/\u04c4/i.test("\u04c6")');
    shouldBeTrue('/\u04c5/i.test("\u04c6")');
    shouldBeTrue('/\u04c6/i.test("\u04c6")');
    shouldBeFalse('/\u04c7/i.test("\u04c6")');
    shouldBeTrue('/[\u04c4-\u04c5]/i.test("\u04c6")');
    shouldBeTrue('/[\u04c6-\u04c7]/i.test("\u04c5")');

    var successfullyParsed = true;
});
