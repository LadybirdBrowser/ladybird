describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.DisplayNames.supportedLocalesOf).toHaveLength(1);
    });

    test("basic functionality", () => {
        // prettier-ignore
        const values = [
            [[], []],
            [undefined, []],
            ["en", ["en"]],
            [new Intl.Locale("en"), ["en"]],
            [["en"], ["en"]],
            [["en", "en-gb", "en-us"], ["en", "en-GB", "en-US"]],
            [["en", "de", "fr"], ["en", "de", "fr"]],
            [["en-foobar"], ["en-foobar"]],
            [["en-foobar-u-abc"], ["en-foobar-u-abc"]],
            [["aa", "zz"], []],
            [["en", "aa", "zz"], ["en"]],
        ];
        for (const [input, expected] of values) {
            expect(Intl.DisplayNames.supportedLocalesOf(input)).toEqual(expected);
            // "best fit" (implementation defined) just uses the same implementation as "lookup" at the moment
            expect(
                Intl.DisplayNames.supportedLocalesOf(input, { localeMatcher: "best fit" })
            ).toEqual(Intl.DisplayNames.supportedLocalesOf(input, { localeMatcher: "lookup" }));
        }
    });
});

describe("errors", () => {
    test("invalid value for localeMatcher option", () => {
        expect(() => {
            Intl.DisplayNames.supportedLocalesOf([], { localeMatcher: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option localeMatcher");
    });

    test("invalid language tag", () => {
        expect(() => {
            Intl.DisplayNames.supportedLocalesOf(["aaaaaaaaa"]);
        }).toThrowWithMessage(RangeError, "aaaaaaaaa is not a structurally valid language tag");
    });
});
