describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.DisplayNames();
        }).toThrowWithMessage(TypeError, "Intl.DisplayNames constructor must be called with 'new'");
    });

    test("options is undefined", () => {
        expect(() => {
            new Intl.DisplayNames("en");
        }).toThrowWithMessage(TypeError, "options is undefined");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.DisplayNames("en", true);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("style option is invalid ", () => {
        expect(() => {
            new Intl.DisplayNames("en", { style: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option style");
    });

    test("type option is invalid ", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option type");
    });

    test("fallback option is invalid ", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "region", fallback: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option fallback");
    });

    test("language display option is invalid ", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "language", languageDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option languageDisplay");
    });

    test("missing type options ", () => {
        expect(() => {
            new Intl.DisplayNames("en", {});
        }).toThrowWithMessage(TypeError, "options.type is undefined");
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Intl.DisplayNames).toHaveLength(2);
    });

    test("all valid types", () => {
        ["language", "region", "script", "currency", "calendar", "dateTimeField"].forEach(type => {
            expect(() => {
                new Intl.DisplayNames("en", { type: type });
            }).not.toThrow();
        });
    });

    test("all valid language displays", () => {
        ["dialect", "standard"].forEach(languageDisplay => {
            expect(() => {
                new Intl.DisplayNames("en", { type: "language", languageDisplay: languageDisplay });
            }).not.toThrow();
        });
    });
});
