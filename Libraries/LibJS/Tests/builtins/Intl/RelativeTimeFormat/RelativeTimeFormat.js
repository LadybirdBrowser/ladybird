describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.RelativeTimeFormat();
        }).toThrowWithMessage(
            TypeError,
            "Intl.RelativeTimeFormat constructor must be called with 'new'"
        );
    });

    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.RelativeTimeFormat("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.RelativeTimeFormat("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.RelativeTimeFormat("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("en", null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("localeMatcher option is invalid", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("numberingSystem option is invalid", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("en", { numberingSystem: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option numberingSystem");
    });

    test("style option is invalid", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("en", { style: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option style");
    });

    test("numeric option is invalid", () => {
        expect(() => {
            new Intl.RelativeTimeFormat("en", { numeric: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option numeric");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.RelativeTimeFormat).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.RelativeTimeFormat("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("valid numberingSystem options", () => {
        ["latn", "arab", "abc-def-ghi"].forEach(numberingSystem => {
            expect(() => {
                new Intl.RelativeTimeFormat("en", { numberingSystem: numberingSystem });
            }).not.toThrow();
        });
    });

    test("all valid style options", () => {
        ["long", "short", "narrow"].forEach(style => {
            expect(() => {
                new Intl.RelativeTimeFormat("en", { style: style });
            }).not.toThrow();
        });
    });

    test("all valid numeric options", () => {
        ["always", "auto"].forEach(numeric => {
            expect(() => {
                new Intl.RelativeTimeFormat("en", { numeric: numeric });
            }).not.toThrow();
        });
    });
});
