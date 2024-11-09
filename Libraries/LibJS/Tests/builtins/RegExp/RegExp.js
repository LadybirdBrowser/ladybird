describe("errors", () => {
    test("invalid pattern", () => {
        expect(() => {
            RegExp("[");
        }).toThrowWithMessage(
            SyntaxError,
            "RegExp compile error: Error during parsing of regular expression:"
        );
    });

    test("invalid flag", () => {
        expect(() => {
            RegExp("", "x");
        }).toThrowWithMessage(SyntaxError, "Invalid RegExp flag 'x'");
    });

    test("repeated flag", () => {
        expect(() => {
            RegExp("", "gg");
        }).toThrowWithMessage(SyntaxError, "Repeated RegExp flag 'g'");
    });
});

test("basic functionality", () => {
    expect(RegExp().toString()).toBe("/(?:)/");
    expect(RegExp(undefined).toString()).toBe("/(?:)/");
    expect(RegExp("foo").toString()).toBe("/foo/");
    expect(RegExp("foo", undefined).toString()).toBe("/foo/");
    expect(RegExp("foo", "g").toString()).toBe("/foo/g");
    expect(RegExp(undefined, "g").toString()).toBe("/(?:)/g");
});

test("regexp object as pattern parameter", () => {
    expect(RegExp(/foo/).toString()).toBe("/foo/");
    expect(RegExp(/foo/g).toString()).toBe("/foo/g");
    expect(RegExp(/foo/g, "").toString()).toBe("/foo/");
    expect(RegExp(/foo/g, "y").toString()).toBe("/foo/y");

    var regex_like_object_without_flags = {
        source: "foo",
        [Symbol.match]: function () {},
    };
    expect(RegExp(regex_like_object_without_flags).toString()).toBe("/foo/");
    expect(RegExp(regex_like_object_without_flags, "y").toString()).toBe("/foo/y");

    var regex_like_object_with_flags = {
        source: "foo",
        flags: "g",
        [Symbol.match]: function () {},
    };
    expect(RegExp(regex_like_object_with_flags).toString()).toBe("/foo/g");
    expect(RegExp(regex_like_object_with_flags, "").toString()).toBe("/foo/");
    expect(RegExp(regex_like_object_with_flags, "y").toString()).toBe("/foo/y");
});

test("regexp literals are re-useable", () => {
    for (var i = 0; i < 2; ++i) {
        const re = /test/;
        expect(re.test("te")).toBeFalse();
        expect(re.test("test")).toBeTrue();
    }
});

test("Incorrectly escaped code units not converted to invalid patterns", () => {
    const re = /[\⪾-\⫀]/;
    expect(re.test("⫀")).toBeTrue();
    expect(re.test("\\u2abe")).toBeFalse(); // ⫀ is \u2abe
});
