test("overflow", () => {
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

    description("This test checks expressions with alternative lengths of appox. 2^31.");

    var regexp1 = /(?:(?=g))|(?:m).{2147483648,}/;
    shouldBe("regexp1.exec('')", "null");

    var regexp2 = /(?:(?=g)).{2147483648,}/;
    shouldBe("regexp2.exec('')", "null");

    var s3 = "&{6}u4a64YfQP{C}u88c4u5772Qu8693{4294967167}u85f2u7f3fs((uf202){4})u5bc6u1947";
    var regexp3 = new RegExp(s3, "");
    shouldBe("regexp3.exec(s3)", "null");

    // Large quantifier values are saturated rather than rejected (matching V8 behavior
    // and the test262 quantifier-integer-limit test which requires accepting 2^53-1).
    shouldNotThrow("function f() { /[^a$]{18446744073709551615}/ }");
    shouldNotThrow("new RegExp('((?=$))??(?:\\\\1){1180591620717411303423,}')");
});
