describe("errors", () => {
    test("called on non-NumberFormat object", () => {
        expect(() => {
            Intl.NumberFormat.prototype.formatRangeToParts();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.NumberFormat");
    });

    test("called without enough values", () => {
        expect(() => {
            new Intl.NumberFormat().formatRangeToParts();
        }).toThrowWithMessage(TypeError, "start is undefined");

        expect(() => {
            new Intl.NumberFormat().formatRangeToParts(1);
        }).toThrowWithMessage(TypeError, "end is undefined");
    });

    test("called with values that cannot be converted to numbers", () => {
        expect(() => {
            new Intl.NumberFormat().formatRangeToParts(Symbol.hasInstance, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Intl.NumberFormat().formatRangeToParts(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("called with invalid numbers", () => {
        expect(() => {
            new Intl.NumberFormat().formatRangeToParts(NaN, 1);
        }).toThrowWithMessage(RangeError, "start must not be NaN");

        expect(() => {
            new Intl.NumberFormat().formatRangeToParts(1, NaN);
        }).toThrowWithMessage(RangeError, "end must not be NaN");
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.formatRangeToParts(100, 101)).toEqual([
            { type: "integer", value: "100", source: "startRange" },
            { type: "literal", value: "–", source: "shared" },
            { type: "integer", value: "101", source: "endRange" },
        ]);

        const ja1 = new Intl.NumberFormat("ja");
        expect(ja1.formatRangeToParts(100, 101)).toEqual([
            { type: "integer", value: "100", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "integer", value: "101", source: "endRange" },
        ]);
    });

    test("approximately formatting", () => {
        const en1 = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
        expect(en1.formatRangeToParts(2.9, 3.1)).toEqual([
            { type: "approximatelySign", value: "~", source: "shared" },
            { type: "integer", value: "3", source: "shared" },
        ]);

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            maximumFractionDigits: 0,
        });
        expect(en2.formatRangeToParts(2.9, 3.1)).toEqual([
            { type: "approximatelySign", value: "~", source: "shared" },
            { type: "currency", value: "$", source: "shared" },
            { type: "integer", value: "3", source: "shared" },
        ]);

        const ja1 = new Intl.NumberFormat("ja", { maximumFractionDigits: 0 });
        expect(ja1.formatRangeToParts(2.9, 3.1)).toEqual([
            { type: "approximatelySign", value: "約", source: "shared" },
            { type: "integer", value: "3", source: "shared" },
        ]);

        const ja2 = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            maximumFractionDigits: 0,
        });
        expect(ja2.formatRangeToParts(2.9, 3.1)).toEqual([
            { type: "approximatelySign", value: "約", source: "shared" },
            { type: "currency", value: "￥", source: "shared" },
            { type: "integer", value: "3", source: "shared" },
        ]);
    });

    test("range pattern spacing", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.formatRangeToParts(3, 5)).toEqual([
            { type: "integer", value: "3", source: "startRange" },
            { type: "literal", value: "–", source: "shared" },
            { type: "integer", value: "5", source: "endRange" },
        ]);

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            maximumFractionDigits: 0,
        });
        expect(en2.formatRangeToParts(3, 5)).toEqual([
            { type: "currency", value: "$", source: "startRange" },
            { type: "integer", value: "3", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "currency", value: "$", source: "endRange" },
            { type: "integer", value: "5", source: "endRange" },
        ]);

        const ja1 = new Intl.NumberFormat("ja");
        expect(ja1.formatRangeToParts(3, 5)).toEqual([
            { type: "integer", value: "3", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "integer", value: "5", source: "endRange" },
        ]);

        const ja2 = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            maximumFractionDigits: 0,
        });
        expect(ja2.formatRangeToParts(3, 5)).toEqual([
            { type: "currency", value: "￥", source: "startRange" },
            { type: "integer", value: "3", source: "startRange" },
            { type: "literal", value: " ～ ", source: "shared" },
            { type: "currency", value: "￥", source: "endRange" },
            { type: "integer", value: "5", source: "endRange" },
        ]);
    });

    test("numbers in reverse order", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatRangeToParts(1, -Infinity)).toEqual([
            { type: "integer", value: "1", source: "startRange" },
            { type: "literal", value: "–", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);
        expect(en.formatRangeToParts(Infinity, -Infinity)).toEqual([
            { type: "infinity", value: "∞", source: "startRange" },
            { type: "literal", value: "–", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);
        expect(en.formatRangeToParts(-0, -Infinity)).toEqual([
            { type: "minusSign", value: "-", source: "startRange" },
            { type: "integer", value: "0", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);

        const ja = new Intl.NumberFormat("ja");
        expect(ja.formatRangeToParts(1, -Infinity)).toEqual([
            { type: "integer", value: "1", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);
        expect(ja.formatRangeToParts(Infinity, -Infinity)).toEqual([
            { type: "infinity", value: "∞", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);
        expect(ja.formatRangeToParts(-0, -Infinity)).toEqual([
            { type: "minusSign", value: "-", source: "startRange" },
            { type: "integer", value: "0", source: "startRange" },
            { type: "literal", value: " ～ ", source: "shared" },
            { type: "minusSign", value: "-", source: "endRange" },
            { type: "infinity", value: "∞", source: "endRange" },
        ]);
    });
});
