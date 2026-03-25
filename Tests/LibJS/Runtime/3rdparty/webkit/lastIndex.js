test("lastIndex", () => {
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

    description("This page tests that a RegExp object's lastIndex behaves like a regular property.");

    // lastIndex is not configurable
    shouldBeFalse("delete /x/.lastIndex");
    shouldThrow("'use strict'; delete /x/.lastIndex");

    // lastIndex is not enumerable
    shouldBeTrue("'lastIndex' in /x/");
    shouldBeTrue("for (property in /x/) if (property === 'lastIndex') throw false; true");

    // lastIndex is writable
    shouldBeTrue("var re = /x/; re.lastIndex = re; re.lastIndex === re");

    // Cannot redefine lastIndex as an accessor
    shouldThrow("Object.defineProperty(/x/, {get:function(){}})");

    // Cannot redefine lastIndex as enumerable
    shouldThrow("Object.defineProperty(/x/, 'lastIndex', {enumerable:true}); true");
    shouldBeTrue("Object.defineProperty(/x/, 'lastIndex', {enumerable:false}); true");

    // Cannot redefine lastIndex as configurable
    shouldThrow("Object.defineProperty(/x/, 'lastIndex', {configurable:true}); true");
    shouldBeTrue("Object.defineProperty(/x/, 'lastIndex', {configurable:false}); true");

    // Can redefine lastIndex as read-only
    shouldBe(
        "var re = Object.defineProperty(/x/, 'lastIndex', {writable:true}); re.lastIndex = 42; re.lastIndex",
        "42"
    );
    shouldBe(
        "var re = Object.defineProperty(/x/, 'lastIndex', {writable:false}); re.lastIndex = 42; re.lastIndex",
        "0"
    );

    // Can redefine the value
    shouldBe("var re = Object.defineProperty(/x/, 'lastIndex', {value:42}); re.lastIndex", "42");

    // Cannot redefine read-only lastIndex as writable
    shouldThrow(
        "Object.defineProperty(Object.defineProperty(/x/, 'lastIndex', {writable:false}), 'lastIndex', {writable:true}); true"
    );

    // Can only redefine the value of a read-only lastIndex as the current value
    shouldThrow(
        "Object.defineProperty(Object.defineProperty(/x/, 'lastIndex', {writable:false}), 'lastIndex', {value:42}); true"
    );
    shouldBeTrue(
        "Object.defineProperty(Object.defineProperty(/x/, 'lastIndex', {writable:false}), 'lastIndex', {value:0}); true"
    );

    // Trying to run a global regular expression should throw, if lastIndex is read-only
    shouldBe("Object.defineProperty(/x/, 'lastIndex', {writable:false}).exec('')", "null");
    shouldBe("Object.defineProperty(/x/, 'lastIndex', {writable:false}).exec('x')", '["x"]');
    shouldThrow("Object.defineProperty(/x/g, 'lastIndex', {writable:false}).exec('')");
    shouldThrow("Object.defineProperty(/x/g, 'lastIndex', {writable:false}).exec('x')");

    // Should be able to freeze a regular expression object.
    shouldBeTrue("var re = /x/; Object.freeze(re); Object.isFrozen(re);");

    // Presence of setter on prototype chain should not affect RegexpMatchesArray
    shouldBe('/x/.exec("x").input', '"x"');
    Object.defineProperty(Object.prototype, "input", { set: function () {} });
    shouldBe('/x/.exec("x").input', '"x"');
});
