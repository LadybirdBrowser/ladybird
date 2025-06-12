describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.ListFormat();
        }).toThrowWithMessage(TypeError, "Intl.ListFormat constructor must be called with 'new'");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.ListFormat("en", true);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("type option is invalid ", () => {
        expect(() => {
            new Intl.ListFormat("en", { type: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option type");
    });

    test("style option is invalid ", () => {
        expect(() => {
            new Intl.ListFormat("en", { style: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option style");
    });

    test("matcher option is invalid ", () => {
        expect(() => {
            new Intl.ListFormat("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.ListFormat).toHaveLength(0);
    });

    test("all valid types", () => {
        ["conjunction", "disjunction", "unit"].forEach(type => {
            expect(() => {
                new Intl.ListFormat("en", { type: type });
            }).not.toThrow();
        });
    });

    test("all valid styles", () => {
        ["long", "short", "narrow"].forEach(style => {
            expect(() => {
                new Intl.ListFormat("en", { style: style });
            }).not.toThrow();
        });
    });

    test("all valid matchers", () => {
        ["lookup", "best fit"].forEach(matcher => {
            expect(() => {
                new Intl.ListFormat("en", { localeMatcher: matcher });
            }).not.toThrow();
        });
    });
});
