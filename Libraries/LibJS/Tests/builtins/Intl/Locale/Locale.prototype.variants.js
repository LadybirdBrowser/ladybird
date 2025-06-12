describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.variants;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });

    test("duplicate variants", () => {
        expect(() => {
            new Intl.Locale("en-abcde-abcde");
        }).toThrowWithMessage(
            RangeError,
            "en-abcde-abcde is not a structurally valid language tag"
        );

        expect(() => {
            new Intl.Locale("en", { variants: "abcde-abcde" });
        }).toThrowWithMessage(RangeError, "abcde-abcde is not a valid value for option variants");
    });

    test("invalid variant", () => {
        expect(() => {
            new Intl.Locale("en-a");
        }).toThrowWithMessage(RangeError, "en-a is not a structurally valid language tag");

        expect(() => {
            new Intl.Locale("en", { variants: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option variants");

        expect(() => {
            new Intl.Locale("en", { variants: "-" });
        }).toThrowWithMessage(RangeError, "- is not a valid value for option variants");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").variants).toBeUndefined();
        expect(new Intl.Locale("en-abcde").variants).toBe("abcde");
        expect(new Intl.Locale("en-1234-abcde").variants).toBe("1234-abcde");
        expect(new Intl.Locale("en", { variants: "abcde" }).variants).toBe("abcde");
        expect(new Intl.Locale("en", { variants: "1234-abcde" }).variants).toBe("1234-abcde");
    });
});
