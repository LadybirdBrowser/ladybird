describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.NumberFormat.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = Intl.NumberFormat("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en2 = Intl.NumberFormat("en-u-nu-latn");
        expect(en2.resolvedOptions().locale).toBe("en-u-nu-latn");

        const en3 = Intl.NumberFormat("en-u-ca-islamicc-nu-latn");
        expect(en3.resolvedOptions().locale).toBe("en-u-nu-latn");
    });

    test("numberingSystem may be set by option", () => {
        const en = Intl.NumberFormat("en", { numberingSystem: "latn" });
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = Intl.NumberFormat("el", { numberingSystem: "latn" });
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem may be set by locale extension", () => {
        const en = Intl.NumberFormat("en-u-nu-latn");
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = Intl.NumberFormat("el-u-nu-latn");
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem option overrides locale extension", () => {
        const el = Intl.NumberFormat("el-u-nu-latn", { numberingSystem: "adlm" });
        expect(el.resolvedOptions().numberingSystem).toBe("adlm");
    });

    test("numberingSystem option limited to known 'nu' values", () => {
        ["latn", "foo"].forEach(numberingSystem => {
            const en = Intl.NumberFormat("en", { numberingSystem: numberingSystem });
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "foo"].forEach(numberingSystem => {
            const en = Intl.NumberFormat(`en-u-nu-${numberingSystem}`);
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = Intl.NumberFormat("el", { numberingSystem: numberingSystem });
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = Intl.NumberFormat(`el-u-nu-${numberingSystem}`);
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });
    });

    test("style", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().style).toBe("decimal");

        ["decimal", "percent", "currency", "unit"].forEach(style => {
            const en2 = new Intl.NumberFormat("en", {
                style: style,
                currency: "USD",
                unit: "degree",
            });
            expect(en2.resolvedOptions().style).toBe(style);
        });
    });

    test("style option of currency sets min/max fraction digits to currency's minor unit", () => {
        const en1 = new Intl.NumberFormat("en", { style: "currency", currency: "USD" });
        expect(en1.resolvedOptions().style).toBe("currency");
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(2);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(2);

        const en2 = new Intl.NumberFormat("en", { style: "currency", currency: "JOD" });
        expect(en2.resolvedOptions().style).toBe("currency");
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(3);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(3);
    });

    test("unknown currency codes and currency codes without minor units default to min/max fraction digits of 2", () => {
        const en1 = new Intl.NumberFormat("en", { style: "currency", currency: "XXX" });
        expect(en1.resolvedOptions().style).toBe("currency");
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(2);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(2);

        const en2 = new Intl.NumberFormat("en", { style: "currency", currency: "XAG" });
        expect(en2.resolvedOptions().style).toBe("currency");
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(2);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(2);
    });

    test("currency digits are not used for non-standard notation", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            notation: "engineering",
        });
        expect(en1.resolvedOptions().style).toBe("currency");
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(3);

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            notation: "compact",
        });
        expect(en2.resolvedOptions().style).toBe("currency");
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(0);
    });

    test("other style options default to min fraction digits of 0 and max fraction digits of 0 or 3", () => {
        const en1 = new Intl.NumberFormat("en", { style: "decimal" });
        expect(en1.resolvedOptions().style).toBe("decimal");
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(3);

        const en2 = new Intl.NumberFormat("en", { style: "unit", unit: "degree" });
        expect(en2.resolvedOptions().style).toBe("unit");
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(3);

        const en3 = new Intl.NumberFormat("en", { style: "percent" });
        expect(en3.resolvedOptions().style).toBe("percent");
        expect(en3.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en3.resolvedOptions().maximumFractionDigits).toBe(0);
    });

    test("min integer digits", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().minimumIntegerDigits).toBe(1);

        const en2 = new Intl.NumberFormat("en", { minimumIntegerDigits: 5 });
        expect(en2.resolvedOptions().minimumIntegerDigits).toBe(5);
    });

    test("min/max fraction digits", () => {
        const en1 = new Intl.NumberFormat("en", { minimumFractionDigits: 5 });
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(5);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(5);
        expect(en1.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en1.resolvedOptions().maximumSignificantDigits).toBeUndefined();

        const en2 = new Intl.NumberFormat("en", { maximumFractionDigits: 5 });
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(5);
        expect(en2.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en2.resolvedOptions().maximumSignificantDigits).toBeUndefined();

        const en3 = new Intl.NumberFormat("en", {
            minimumFractionDigits: 5,
            maximumFractionDigits: 10,
        });
        expect(en3.resolvedOptions().minimumFractionDigits).toBe(5);
        expect(en3.resolvedOptions().maximumFractionDigits).toBe(10);
        expect(en3.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en3.resolvedOptions().maximumSignificantDigits).toBeUndefined();
    });

    test("min/max significant digits", () => {
        const en1 = new Intl.NumberFormat("en", { minimumSignificantDigits: 5 });
        expect(en1.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en1.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en1.resolvedOptions().minimumSignificantDigits).toBe(5);
        expect(en1.resolvedOptions().maximumSignificantDigits).toBe(21);

        const en2 = new Intl.NumberFormat("en", { maximumSignificantDigits: 5 });
        expect(en2.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en2.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en2.resolvedOptions().minimumSignificantDigits).toBe(1);
        expect(en2.resolvedOptions().maximumSignificantDigits).toBe(5);

        const en3 = new Intl.NumberFormat("en", {
            minimumSignificantDigits: 5,
            maximumSignificantDigits: 10,
        });
        expect(en3.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en3.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en3.resolvedOptions().minimumSignificantDigits).toBe(5);
        expect(en3.resolvedOptions().maximumSignificantDigits).toBe(10);
    });

    test("notation", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().notation).toBe("standard");

        ["standard", "scientific", "engineering", "compact"].forEach(notation => {
            const en2 = new Intl.NumberFormat("en", { notation: notation });
            expect(en2.resolvedOptions().notation).toBe(notation);
        });
    });

    test("compact notation causes all min/max digits to be set to default values", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact" });
        expect(en.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en.resolvedOptions().maximumFractionDigits).toBe(0);
        expect(en.resolvedOptions().minimumSignificantDigits).toBe(1);
        expect(en.resolvedOptions().maximumSignificantDigits).toBe(2);
    });

    test("currency display and sign only defined when style is currency", () => {
        ["decimal", "percent", "unit"].forEach(style => {
            const en1 = new Intl.NumberFormat("en", { style: style, unit: "degree" });
            expect(en1.resolvedOptions().currencyDisplay).toBeUndefined();
            expect(en1.resolvedOptions().currencySign).toBeUndefined();

            const en2 = new Intl.NumberFormat("en", {
                style: style,
                unit: "degree",
                currencyDisplay: "symbol",
                currencySign: "standard",
            });
            expect(en2.resolvedOptions().style).toBe(style);
            expect(en2.resolvedOptions().currencyDisplay).toBeUndefined();
            expect(en2.resolvedOptions().currencySign).toBeUndefined();
        });

        const en3 = new Intl.NumberFormat("en", { style: "currency", currency: "USD" });
        expect(en3.resolvedOptions().currencyDisplay).toBe("symbol");
        expect(en3.resolvedOptions().currencySign).toBe("standard");

        ["code", "symbol", "narrowSymbol", "name"].forEach(currencyDisplay => {
            ["standard", "accounting"].forEach(currencySign => {
                const en4 = new Intl.NumberFormat("en", {
                    style: "currency",
                    currency: "USD",
                    currencyDisplay: currencyDisplay,
                    currencySign: currencySign,
                });
                expect(en4.resolvedOptions().style).toBe("currency");
                expect(en4.resolvedOptions().currencyDisplay).toBe(currencyDisplay);
                expect(en4.resolvedOptions().currencySign).toBe(currencySign);
            });
        });
    });

    test("unit display only defined when style is unit", () => {
        ["decimal", "percent", "currency"].forEach(style => {
            const en1 = new Intl.NumberFormat("en", { style: style, currency: "USD" });
            expect(en1.resolvedOptions().unitDisplay).toBeUndefined();

            const en2 = new Intl.NumberFormat("en", {
                style: style,
                currency: "USD",
                unitDisplay: "short",
            });
            expect(en2.resolvedOptions().style).toBe(style);
            expect(en2.resolvedOptions().unitDisplay).toBeUndefined();
        });

        const en3 = new Intl.NumberFormat("en", { style: "unit", unit: "degree" });
        expect(en3.resolvedOptions().unitDisplay).toBe("short");

        ["short", "narrow", "long"].forEach(unitDisplay => {
            const en4 = new Intl.NumberFormat("en", {
                style: "unit",
                unit: "degree",
                unitDisplay: unitDisplay,
            });
            expect(en4.resolvedOptions().style).toBe("unit");
            expect(en4.resolvedOptions().unitDisplay).toBe(unitDisplay);
        });
    });

    test("compact display only defined when notation is compact", () => {
        ["standard", "scientific", "engineering"].forEach(notation => {
            const en1 = new Intl.NumberFormat("en", { notation: notation });
            expect(en1.resolvedOptions().compactDisplay).toBeUndefined();

            const en2 = new Intl.NumberFormat("en", {
                notation: notation,
                compactDisplay: "short",
            });
            expect(en2.resolvedOptions().notation).toBe(notation);
            expect(en2.resolvedOptions().compactDisplay).toBeUndefined();
        });

        const en3 = new Intl.NumberFormat("en", { notation: "compact" });
        expect(en3.resolvedOptions().compactDisplay).toBe("short");

        ["short", "long"].forEach(compactDisplay => {
            const en4 = new Intl.NumberFormat("en", {
                notation: "compact",
                compactDisplay: compactDisplay,
            });
            expect(en4.resolvedOptions().notation).toBe("compact");
            expect(en4.resolvedOptions().compactDisplay).toBe(compactDisplay);
        });
    });

    test("use grouping", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().useGrouping).toBe("auto");

        const en2 = new Intl.NumberFormat("en", { notation: "compact" });
        expect(en2.resolvedOptions().useGrouping).toBe("min2");

        const en3 = new Intl.NumberFormat("en", { useGrouping: false });
        expect(en3.resolvedOptions().useGrouping).toBeFalse();

        const en4 = new Intl.NumberFormat("en", { useGrouping: true });
        expect(en4.resolvedOptions().useGrouping).toBe("always");

        const en5 = new Intl.NumberFormat("en", { useGrouping: "false" });
        expect(en5.resolvedOptions().useGrouping).toBe("auto");

        const en6 = new Intl.NumberFormat("en", { useGrouping: "true" });
        expect(en6.resolvedOptions().useGrouping).toBe("auto");

        ["auto", "always", "min2"].forEach(useGrouping => {
            const en7 = new Intl.NumberFormat("en", { useGrouping: useGrouping });
            expect(en7.resolvedOptions().useGrouping).toBe(useGrouping);
        });
    });

    test("sign display", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().signDisplay).toBe("auto");

        ["auto", "never", "always", "exceptZero", "negative"].forEach(signDisplay => {
            const en2 = new Intl.NumberFormat("en", { signDisplay: signDisplay });
            expect(en2.resolvedOptions().signDisplay).toBe(signDisplay);
        });
    });

    test("rounding priority", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().roundingPriority).toBe("auto");

        const en2 = new Intl.NumberFormat("en", { notation: "compact" });
        expect(en2.resolvedOptions().roundingPriority).toBe("morePrecision");

        ["auto", "morePrecision", "lessPrecision"].forEach(roundingPriority => {
            const en3 = new Intl.NumberFormat("en", { roundingPriority: roundingPriority });
            expect(en3.resolvedOptions().roundingPriority).toBe(roundingPriority);
        });
    });

    test("rounding mode", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().roundingMode).toBe("halfExpand");

        [
            "ceil",
            "floor",
            "expand",
            "trunc",
            "halfCeil",
            "halfFloor",
            "halfExpand",
            "halfTrunc",
            "halfEven",
        ].forEach(roundingMode => {
            const en2 = new Intl.NumberFormat("en", { roundingMode: roundingMode });
            expect(en2.resolvedOptions().roundingMode).toBe(roundingMode);
        });
    });

    test("rounding increment", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().roundingIncrement).toBe(1);

        [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000].forEach(
            roundingIncrement => {
                const en2 = new Intl.NumberFormat("en", { roundingIncrement: roundingIncrement });
                expect(en2.resolvedOptions().roundingIncrement).toBe(roundingIncrement);
            }
        );
    });

    test("trailing zero display", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.resolvedOptions().trailingZeroDisplay).toBe("auto");

        ["auto", "stripIfInteger"].forEach(trailingZeroDisplay => {
            const en2 = new Intl.NumberFormat("en", { trailingZeroDisplay: trailingZeroDisplay });
            expect(en2.resolvedOptions().trailingZeroDisplay).toBe(trailingZeroDisplay);
        });
    });
});
