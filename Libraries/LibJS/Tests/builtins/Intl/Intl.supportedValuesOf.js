describe("errors", () => {
    test("invalid key", () => {
        expect(() => {
            Intl.supportedValuesOf("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid key");
    });
});

describe("normal behavior", () => {
    const isSorted = array => {
        return array.slice(1).every((item, i) => array[i] <= item);
    };

    test("length is 1", () => {
        expect(Intl.supportedValuesOf).toHaveLength(1);
    });

    test("calendar", () => {
        const values = Intl.supportedValuesOf("calendar");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("gregory")).not.toBe(-1);
    });

    test("collation", () => {
        const values = Intl.supportedValuesOf("collation");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("default")).not.toBe(-1);
    });

    test("currency", () => {
        const values = Intl.supportedValuesOf("currency");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("USD")).not.toBe(-1);
        expect(values.indexOf("XXX")).not.toBe(-1);
    });

    test("numberingSystem", () => {
        const values = Intl.supportedValuesOf("numberingSystem");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("latn")).not.toBe(-1);
        expect(values.indexOf("arab")).not.toBe(-1);
    });

    test("timeZone", () => {
        const values = Intl.supportedValuesOf("timeZone");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("UTC")).not.toBe(-1);
        expect(values.indexOf("America/New_York")).not.toBe(-1);
    });

    test("unit", () => {
        const values = Intl.supportedValuesOf("unit");
        expect(isSorted(values)).toBeTrue();

        expect(values.indexOf("acre")).not.toBe(-1);
        expect(values.indexOf("fluid-ounce")).not.toBe(-1);
    });
});
