describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => Number.prototype.toLocaleString.call(value)).toThrowWithMessage(
                TypeError,
                "Not an object of type Number"
            );
        });
    });
});

describe("correct behavior", () => {
    test("length", () => {
        expect(Number.prototype.toLocaleString).toHaveLength(0);
    });
});

describe("special values", () => {
    test("NaN", () => {
        expect(NaN.toLocaleString()).toBe("NaN");
        expect(NaN.toLocaleString("en")).toBe("NaN");
        expect(NaN.toLocaleString("ar")).toBe("ليس رقم");
    });

    test("Infinity", () => {
        expect(Infinity.toLocaleString()).toBe("∞");
        expect(Infinity.toLocaleString("en")).toBe("∞");
        expect(Infinity.toLocaleString("ar")).toBe("∞");
    });
});

describe("styles", () => {
    test("decimal", () => {
        expect((12).toLocaleString("en")).toBe("12");
        expect((12).toLocaleString("ar")).toBe("\u0661\u0662");
    });

    test("percent", () => {
        expect((0.234).toLocaleString("en", { style: "percent" })).toBe("23%");
        expect((0.234).toLocaleString("ar", { style: "percent" })).toBe("\u0662\u0663\u066a\u061c");
    });

    test("currency", () => {
        expect(
            (1.23).toLocaleString("en", {
                style: "currency",
                currency: "USD",
                currencyDisplay: "name",
            })
        ).toBe("1.23 US dollars");

        expect(
            (1.23).toLocaleString("ar", {
                style: "currency",
                currency: "USD",
                currencyDisplay: "name",
            })
        ).toBe("\u0661\u066b\u0662\u0663 دولار أمريكي");
    });

    test("unit", () => {
        expect(
            (1.23).toLocaleString("en", {
                style: "unit",
                unit: "kilometer-per-hour",
                unitDisplay: "long",
            })
        ).toBe("1.23 kilometers per hour");

        expect(
            (1.23).toLocaleString("ar", {
                style: "unit",
                unit: "kilometer-per-hour",
                unitDisplay: "long",
            })
        ).toBe("\u0661\u066b\u0662\u0663 كيلومتر في الساعة");
    });
});
