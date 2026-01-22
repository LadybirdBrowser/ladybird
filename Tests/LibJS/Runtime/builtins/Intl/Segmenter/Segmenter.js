describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.Segmenter();
        }).toThrowWithMessage(TypeError, "Intl.Segmenter constructor must be called with 'new'");
    });

    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.Segmenter("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.Segmenter("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.Segmenter("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.Segmenter("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.Segmenter("en", null);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("localeMatcher option is invalid", () => {
        expect(() => {
            new Intl.Segmenter("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("granularity option is invalid", () => {
        expect(() => {
            new Intl.Segmenter("en", { granularity: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option granularity");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.Segmenter).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.Segmenter("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("all valid granularity options", () => {
        ["grapheme", "word", "sentence"].forEach(granularity => {
            expect(() => {
                new Intl.Segmenter("en", { granularity: granularity });
            }).not.toThrow();
        });
    });
});
