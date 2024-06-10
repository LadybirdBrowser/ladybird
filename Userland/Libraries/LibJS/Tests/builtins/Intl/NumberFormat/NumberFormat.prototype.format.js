describe("errors", () => {
    test("called on non-NumberFormat object", () => {
        expect(() => {
            Intl.NumberFormat.prototype.format;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.NumberFormat");

        expect(() => {
            Intl.NumberFormat.prototype.format(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.NumberFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            Intl.NumberFormat().format(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("special values", () => {
    test("NaN", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.format()).toBe("NaN");
        expect(en.format(NaN)).toBe("NaN");
        expect(en.format(undefined)).toBe("NaN");

        const ar = new Intl.NumberFormat("ar");
        expect(ar.format()).toBe("ليس رقم");
        expect(ar.format(NaN)).toBe("ليس رقم");
        expect(ar.format(undefined)).toBe("ليس رقم");
    });

    test("Infinity", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.format(Infinity)).toBe("∞");
        expect(en.format(-Infinity)).toBe("-∞");

        const ar = new Intl.NumberFormat("ar");
        expect(ar.format(Infinity)).toBe("∞");
        expect(ar.format(-Infinity)).toBe("\u061c-∞");
    });
});

describe("style=decimal", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.format(1)).toBe("1");
        expect(en.format(12)).toBe("12");
        expect(en.format(123)).toBe("123");
        expect(en.format("987654321987654321")).toBe("987,654,321,987,654,321");

        const ar = new Intl.NumberFormat("ar");
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(12)).toBe("\u0661\u0662");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
        expect(ar.format("987654321987654321")).toBe(
            "\u0669\u0668\u0667\u066c\u0666\u0665\u0664\u066c\u0663\u0662\u0661\u066c\u0669\u0668\u0667\u066c\u0666\u0665\u0664\u066c\u0663\u0662\u0661"
        );
    });

    test("integer digits", () => {
        const en = new Intl.NumberFormat("en", { minimumIntegerDigits: 2 });
        expect(en.format(1)).toBe("01");
        expect(en.format(12)).toBe("12");
        expect(en.format(123)).toBe("123");

        const ar = new Intl.NumberFormat("ar", { minimumIntegerDigits: 2 });
        expect(ar.format(1)).toBe("\u0660\u0661");
        expect(ar.format(12)).toBe("\u0661\u0662");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
    });

    test("significant digits", () => {
        const en = new Intl.NumberFormat("en", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(en.format(1)).toBe("1.000");
        expect(en.format(12)).toBe("12.00");
        expect(en.format(12.3)).toBe("12.30");
        expect(en.format(12.34)).toBe("12.34");
        expect(en.format(12.345)).toBe("12.345");
        expect(en.format(12.3456)).toBe("12.3456");
        expect(en.format(12.34567)).toBe("12.3457");
        expect(en.format(12.34561)).toBe("12.3456");
        expect(en.format(0.00000000000000000000000000000123)).toBe(
            "0.000000000000000000000000000001230"
        );
        expect(en.format(-0.00000000000000000000000000000123)).toBe(
            "-0.000000000000000000000000000001230"
        );
        expect(en.format(12344501000000000000000000000000000)).toBe(
            "12,344,500,000,000,000,000,000,000,000,000,000"
        );
        expect(en.format(-12344501000000000000000000000000000)).toBe(
            "-12,344,500,000,000,000,000,000,000,000,000,000"
        );
        expect(en.format(12344501000000000000000000000000000n)).toBe(
            "12,344,500,000,000,000,000,000,000,000,000,000"
        );
        expect(en.format(-12344501000000000000000000000000000n)).toBe(
            "-12,344,500,000,000,000,000,000,000,000,000,000"
        );

        const enLargeMaxSignificantDigits = new Intl.NumberFormat("en", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 21,
        });
        expect(enLargeMaxSignificantDigits.format(1)).toBe("1.000");
        expect(enLargeMaxSignificantDigits.format(1n)).toBe("1.000");
        expect(enLargeMaxSignificantDigits.format(123456789123456789123456789123456789n)).toBe(
            "123,456,789,123,456,789,123,000,000,000,000,000"
        );

        const ar = new Intl.NumberFormat("ar", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(ar.format(1)).toBe("\u0661\u066b\u0660\u0660\u0660");
        expect(ar.format(12)).toBe("\u0661\u0662\u066b\u0660\u0660");
        expect(ar.format(12.3)).toBe("\u0661\u0662\u066b\u0663\u0660");
        expect(ar.format(12.34)).toBe("\u0661\u0662\u066b\u0663\u0664");
        expect(ar.format(12.345)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665");
        expect(ar.format(12.3456)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0666");
        expect(ar.format(12.34567)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0667");
        expect(ar.format(12.34561)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0666");
    });

    test("fraction digits", () => {
        const en = new Intl.NumberFormat("en", {
            minimumFractionDigits: 3,
            maximumFractionDigits: 5,
        });
        expect(en.format(1)).toBe("1.000");
        expect(en.format(12)).toBe("12.000");
        expect(en.format(1.2)).toBe("1.200");
        expect(en.format(1.23)).toBe("1.230");
        expect(en.format(1.234)).toBe("1.234");
        expect(en.format(1.2345)).toBe("1.2345");
        expect(en.format(1.23456)).toBe("1.23456");
        expect(en.format(1.234567)).toBe("1.23457");
        expect(en.format(1.234561)).toBe("1.23456");
        expect(en.format("12344501000000000000000000000000000")).toBe(
            "12,344,501,000,000,000,000,000,000,000,000,000.000"
        );
        expect(en.format("-12344501000000000000000000000000000")).toBe(
            "-12,344,501,000,000,000,000,000,000,000,000,000.000"
        );

        const ar = new Intl.NumberFormat("ar", {
            minimumFractionDigits: 3,
            maximumFractionDigits: 5,
        });
        expect(ar.format(1)).toBe("\u0661\u066b\u0660\u0660\u0660");
        expect(ar.format(12)).toBe("\u0661\u0662\u066b\u0660\u0660\u0660");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662\u0660\u0660");
        expect(ar.format(1.23)).toBe("\u0661\u066b\u0662\u0663\u0660");
        expect(ar.format(1.234)).toBe("\u0661\u066b\u0662\u0663\u0664");
        expect(ar.format(1.2345)).toBe("\u0661\u066b\u0662\u0663\u0664\u0665");
        expect(ar.format(1.23456)).toBe("\u0661\u066b\u0662\u0663\u0664\u0665\u0666");
        expect(ar.format(1.234567)).toBe("\u0661\u066b\u0662\u0663\u0664\u0665\u0667");
        expect(ar.format(1.234561)).toBe("\u0661\u066b\u0662\u0663\u0664\u0665\u0666");

        let digits = "\u0661\u0662\u066c\u0663\u0664\u0664\u066c\u0665\u0660\u0661";
        digits += "\u066c\u0660\u0660\u0660".repeat(9);
        digits += "\u066b\u0660\u0660\u0660";

        expect(ar.format("12344501000000000000000000000000000")).toBe(digits);
        expect(ar.format("-12344501000000000000000000000000000")).toBe("\u061c-" + digits);
    });

    test("notation=scientific", () => {
        const en = new Intl.NumberFormat("en", { notation: "scientific" });
        expect(en.format(1)).toBe("1E0");
        expect(en.format(1.2)).toBe("1.2E0");
        expect(en.format(12)).toBe("1.2E1");
        expect(en.format(12.3)).toBe("1.23E1");
        expect(en.format(123)).toBe("1.23E2");
        expect(en.format(0.1)).toBe("1E-1");
        expect(en.format(0.12)).toBe("1.2E-1");
        expect(en.format(0.01)).toBe("1E-2");

        const ar = new Intl.NumberFormat("ar", { notation: "scientific" });
        expect(ar.format(1)).toBe("\u0661\u0623\u0633\u0660");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662\u0623\u0633\u0660");
        expect(ar.format(12)).toBe("\u0661\u066b\u0662\u0623\u0633\u0661");
        expect(ar.format(12.3)).toBe("\u0661\u066b\u0662\u0663\u0623\u0633\u0661");
        expect(ar.format(123)).toBe("\u0661\u066b\u0662\u0663\u0623\u0633\u0662");
        expect(ar.format(0.1)).toBe("\u0661\u0623\u0633\u061c-\u0661");
        expect(ar.format(0.12)).toBe("\u0661\u066b\u0662\u0623\u0633\u061c-\u0661");
        expect(ar.format(0.01)).toBe("\u0661\u0623\u0633\u061c-\u0662");
    });

    test("notation=engineering", () => {
        const en = new Intl.NumberFormat("en", { notation: "engineering" });
        expect(en.format(1)).toBe("1E0");
        expect(en.format(1.2)).toBe("1.2E0");
        expect(en.format(12)).toBe("12E0");
        expect(en.format(123)).toBe("123E0");
        expect(en.format(1234)).toBe("1.234E3");
        expect(en.format(12345)).toBe("12.345E3");
        expect(en.format(123456)).toBe("123.456E3");
        expect(en.format(1234567)).toBe("1.235E6");
        expect(en.format(0.1)).toBe("100E-3");
        expect(en.format(0.12)).toBe("120E-3");
        expect(en.format(1.23)).toBe("1.23E0");

        const ar = new Intl.NumberFormat("ar", { notation: "engineering" });
        expect(ar.format(1)).toBe("\u0661\u0623\u0633\u0660");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662\u0623\u0633\u0660");
        expect(ar.format(12)).toBe("\u0661\u0662\u0623\u0633\u0660");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663\u0623\u0633\u0660");
        expect(ar.format(1234)).toBe("\u0661\u066b\u0662\u0663\u0664\u0623\u0633\u0663");
        expect(ar.format(12345)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0623\u0633\u0663");
        expect(ar.format(123456)).toBe(
            "\u0661\u0662\u0663\u066b\u0664\u0665\u0666\u0623\u0633\u0663"
        );
        expect(ar.format(1234567)).toBe("\u0661\u066b\u0662\u0663\u0665\u0623\u0633\u0666");
        expect(ar.format(0.1)).toBe("\u0661\u0660\u0660\u0623\u0633\u061c-\u0663");
        expect(ar.format(0.12)).toBe("\u0661\u0662\u0660\u0623\u0633\u061c-\u0663");
        expect(ar.format(1.23)).toBe("\u0661\u066b\u0662\u0663\u0623\u0633\u0660");
    });

    test("notation=compact", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact" });
        expect(en.format(1)).toBe("1");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(1.23)).toBe("1.2");
        expect(en.format(1.29)).toBe("1.3");
        expect(en.format(12)).toBe("12");
        expect(en.format(12.3)).toBe("12");
        expect(en.format(12.34)).toBe("12");

        const ar = new Intl.NumberFormat("ar", { notation: "compact" });
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.23)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.29)).toBe("\u0661\u066b\u0663");
        expect(ar.format(12)).toBe("\u0661\u0662");
        expect(ar.format(12.3)).toBe("\u0661\u0662");
        expect(ar.format(12.34)).toBe("\u0661\u0662");
    });

    test("notation=compact, compactDisplay=long", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact", compactDisplay: "long" });
        expect(en.format(1)).toBe("1");
        expect(en.format(1200)).toBe("1.2 thousand");
        expect(en.format(1290)).toBe("1.3 thousand");
        expect(en.format(12000)).toBe("12 thousand");
        expect(en.format(12900)).toBe("13 thousand");
        expect(en.format(1200000)).toBe("1.2 million");
        expect(en.format(1290000)).toBe("1.3 million");
        expect(en.format(12000000)).toBe("12 million");
        expect(en.format(12900000)).toBe("13 million");

        const enFullwide = new Intl.NumberFormat("en", {
            notation: "compact",
            compactDisplay: "long",
            numberingSystem: "fullwide",
        });
        expect(enFullwide.format(1)).toBe("１");
        expect(enFullwide.format(1200)).toBe("１.２ thousand");
        expect(enFullwide.format(1290)).toBe("１.３ thousand");
        expect(enFullwide.format(12000)).toBe("１２ thousand");
        expect(enFullwide.format(12900)).toBe("１３ thousand");
        expect(enFullwide.format(1200000)).toBe("１.２ million");
        expect(enFullwide.format(1290000)).toBe("１.３ million");
        expect(enFullwide.format(12000000)).toBe("１２ million");
        expect(enFullwide.format(12900000)).toBe("１３ million");

        const ar = new Intl.NumberFormat("ar", { notation: "compact", compactDisplay: "long" });
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(1200)).toBe("\u0661\u066b\u0662 ألف");
        expect(ar.format(1290)).toBe("\u0661\u066b\u0663 ألف");
        expect(ar.format(12000)).toBe("\u0661\u0662 ألف");
        expect(ar.format(12900)).toBe("\u0661\u0663 ألف");
        expect(ar.format(1200000)).toBe("\u0661\u066b\u0662 مليون");
        expect(ar.format(1290000)).toBe("\u0661\u066b\u0663 مليون");
        expect(ar.format(12000000)).toBe("\u0661\u0662 مليون");
        expect(ar.format(12900000)).toBe("\u0661\u0663 مليون");

        const ja = new Intl.NumberFormat("ja", { notation: "compact", compactDisplay: "long" });
        expect(ja.format(1)).toBe("1");
        expect(ja.format(1200)).toBe("1200");
        expect(ja.format(1290)).toBe("1290");
        expect(ja.format(12000)).toBe("1.2万");
        expect(ja.format(12900)).toBe("1.3万");
        expect(ja.format(1200000)).toBe("120万");
        expect(ja.format(1290000)).toBe("129万");
        expect(ja.format(12000000)).toBe("1200万");
        expect(ja.format(12900000)).toBe("1290万");
        expect(ja.format(120000000)).toBe("1.2億");
        expect(ja.format(129000000)).toBe("1.3億");
        expect(ja.format(12000000000)).toBe("120億");
        expect(ja.format(12900000000)).toBe("129億");

        const de = new Intl.NumberFormat("de", { notation: "compact", compactDisplay: "long" });
        expect(de.format(1)).toBe("1");
        expect(de.format(1200)).toBe("1,2 Tausend");
        expect(de.format(1290)).toBe("1,3 Tausend");
        expect(de.format(12000)).toBe("12 Tausend");
        expect(de.format(12900)).toBe("13 Tausend");
        expect(de.format(1200000)).toBe("1,2 Millionen");
        expect(de.format(1290000)).toBe("1,3 Millionen");
        expect(de.format(12000000)).toBe("12 Millionen");
        expect(de.format(12900000)).toBe("13 Millionen");
    });

    test("notation=compact, compactDisplay=short", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact", compactDisplay: "short" });
        expect(en.format(1)).toBe("1");
        expect(en.format(1200)).toBe("1.2K");
        expect(en.format(1290)).toBe("1.3K");
        expect(en.format(12000)).toBe("12K");
        expect(en.format(12900)).toBe("13K");
        expect(en.format(1200000)).toBe("1.2M");
        expect(en.format(1290000)).toBe("1.3M");
        expect(en.format(12000000)).toBe("12M");
        expect(en.format(12900000)).toBe("13M");

        const ar = new Intl.NumberFormat("ar", { notation: "compact", compactDisplay: "short" });
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(1200)).toBe("\u0661\u066b\u0662\u00a0ألف");
        expect(ar.format(1290)).toBe("\u0661\u066b\u0663\u00a0ألف");
        expect(ar.format(12000)).toBe("\u0661\u0662\u00a0ألف");
        expect(ar.format(12900)).toBe("\u0661\u0663\u00a0ألف");
        expect(ar.format(1200000)).toBe("\u0661\u066b\u0662\u00a0مليون");
        expect(ar.format(1290000)).toBe("\u0661\u066b\u0663\u00a0مليون");
        expect(ar.format(12000000)).toBe("\u0661\u0662\u00a0مليون");
        expect(ar.format(12900000)).toBe("\u0661\u0663\u00a0مليون");

        const ja = new Intl.NumberFormat("ja", { notation: "compact", compactDisplay: "short" });
        expect(ja.format(1)).toBe("1");
        expect(ja.format(1200)).toBe("1200");
        expect(ja.format(1290)).toBe("1290");
        expect(ja.format(12000)).toBe("1.2万");
        expect(ja.format(12900)).toBe("1.3万");
        expect(ja.format(1200000)).toBe("120万");
        expect(ja.format(1290000)).toBe("129万");
        expect(ja.format(12000000)).toBe("1200万");
        expect(ja.format(12900000)).toBe("1290万");
        expect(ja.format(120000000)).toBe("1.2億");
        expect(ja.format(129000000)).toBe("1.3億");
        expect(ja.format(12000000000)).toBe("120億");
        expect(ja.format(12900000000)).toBe("129億");

        const de = new Intl.NumberFormat("de", { notation: "compact", compactDisplay: "short" });
        expect(de.format(1)).toBe("1");
        expect(de.format(1200)).toBe("1200");
        expect(de.format(1290)).toBe("1290");
        expect(de.format(12000)).toBe("12.000");
        expect(de.format(12900)).toBe("12.900");
        expect(de.format(1200000)).toBe("1,2\u00a0Mio.");
        expect(de.format(1290000)).toBe("1,3\u00a0Mio.");
        expect(de.format(12000000)).toBe("12\u00a0Mio.");
        expect(de.format(12900000)).toBe("13\u00a0Mio.");
    });

    test("signDisplay=never", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "never" });
        expect(en.format(1)).toBe("1");
        expect(en.format(-1)).toBe("1");

        const ar = new Intl.NumberFormat("ar", { signDisplay: "never" });
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(-1)).toBe("\u0661");
    });

    test("signDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "auto" });
        expect(en.format(0)).toBe("0");
        expect(en.format(1)).toBe("1");
        expect(en.format(-0)).toBe("-0");
        expect(en.format(-1)).toBe("-1");

        const ar = new Intl.NumberFormat("ar", { signDisplay: "auto" });
        expect(ar.format(0)).toBe("\u0660");
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(-0)).toBe("\u061c-\u0660");
        expect(ar.format(-1)).toBe("\u061c-\u0661");
    });

    test("signDisplay=always", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "always" });
        expect(en.format(0)).toBe("+0");
        expect(en.format(1)).toBe("+1");
        expect(en.format(-0)).toBe("-0");
        expect(en.format(-1)).toBe("-1");

        const ar = new Intl.NumberFormat("ar", { signDisplay: "always" });
        expect(ar.format(0)).toBe("\u061c+\u0660");
        expect(ar.format(1)).toBe("\u061c+\u0661");
        expect(ar.format(-0)).toBe("\u061c-\u0660");
        expect(ar.format(-1)).toBe("\u061c-\u0661");
    });

    test("signDisplay=exceptZero", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "exceptZero" });
        expect(en.format(0)).toBe("0");
        expect(en.format(1)).toBe("+1");
        expect(en.format(-0)).toBe("0");
        expect(en.format(-1)).toBe("-1");

        const ar = new Intl.NumberFormat("ar", { signDisplay: "exceptZero" });
        expect(ar.format(0)).toBe("\u0660");
        expect(ar.format(1)).toBe("\u061c+\u0661");
        expect(ar.format(-0)).toBe("\u0660");
        expect(ar.format(-1)).toBe("\u061c-\u0661");
    });

    test("signDisplay=negative", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "negative" });
        expect(en.format(0)).toBe("0");
        expect(en.format(1)).toBe("1");
        expect(en.format(-0)).toBe("0");
        expect(en.format(-1)).toBe("-1");

        const ar = new Intl.NumberFormat("ar", { signDisplay: "negative" });
        expect(ar.format(0)).toBe("\u0660");
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(-0)).toBe("\u0660");
        expect(ar.format(-1)).toBe("\u061c-\u0661");
    });

    test("useGrouping=always", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "always" });
        expect(en.format(123)).toBe("123");
        expect(en.format(1234)).toBe("1,234");
        expect(en.format(12345)).toBe("12,345");
        expect(en.format(123456)).toBe("123,456");
        expect(en.format(1234567)).toBe("1,234,567");

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: "always" });
        expect(enIn.format(123)).toBe("123");
        expect(enIn.format(1234)).toBe("1,234");
        expect(enIn.format(12345)).toBe("12,345");
        expect(enIn.format(123456)).toBe("1,23,456");
        expect(enIn.format(1234567)).toBe("12,34,567");

        const ar = new Intl.NumberFormat("ar", { useGrouping: "always" });
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
        expect(ar.format(1234)).toBe("\u0661\u066c\u0662\u0663\u0664");
        expect(ar.format(12345)).toBe("\u0661\u0662\u066c\u0663\u0664\u0665");
        expect(ar.format(123456)).toBe("\u0661\u0662\u0663\u066c\u0664\u0665\u0666");
        expect(ar.format(1234567)).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0666\u0667");

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: "always" });
        expect(plPl.format(123)).toBe("123");
        expect(plPl.format(1234)).toBe("1\u00a0234");
        expect(plPl.format(12345)).toBe("12\u00a0345");
        expect(plPl.format(123456)).toBe("123\u00a0456");
        expect(plPl.format(1234567)).toBe("1\u00a0234\u00a0567");
    });

    test("useGrouping=auto", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "auto" });
        expect(en.format(123)).toBe("123");
        expect(en.format(1234)).toBe("1,234");
        expect(en.format(12345)).toBe("12,345");
        expect(en.format(123456)).toBe("123,456");
        expect(en.format(1234567)).toBe("1,234,567");

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: "auto" });
        expect(enIn.format(123)).toBe("123");
        expect(enIn.format(1234)).toBe("1,234");
        expect(enIn.format(12345)).toBe("12,345");
        expect(enIn.format(123456)).toBe("1,23,456");
        expect(enIn.format(1234567)).toBe("12,34,567");

        const ar = new Intl.NumberFormat("ar", { useGrouping: "auto" });
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
        expect(ar.format(1234)).toBe("\u0661\u066c\u0662\u0663\u0664");
        expect(ar.format(12345)).toBe("\u0661\u0662\u066c\u0663\u0664\u0665");
        expect(ar.format(123456)).toBe("\u0661\u0662\u0663\u066c\u0664\u0665\u0666");
        expect(ar.format(1234567)).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0666\u0667");

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: "auto" });
        expect(plPl.format(123)).toBe("123");
        expect(plPl.format(1234)).toBe("1234");
        expect(plPl.format(12345)).toBe("12\u00a0345");
        expect(plPl.format(123456)).toBe("123\u00a0456");
        expect(plPl.format(1234567)).toBe("1\u00a0234\u00a0567");
    });

    test("useGrouping=min2", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "min2" });
        expect(en.format(123)).toBe("123");
        expect(en.format(1234)).toBe("1234");
        expect(en.format(12345)).toBe("12,345");
        expect(en.format(123456)).toBe("123,456");
        expect(en.format(1234567)).toBe("1,234,567");

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: "min2" });
        expect(enIn.format(123)).toBe("123");
        expect(enIn.format(1234)).toBe("1234");
        expect(enIn.format(12345)).toBe("12,345");
        expect(enIn.format(123456)).toBe("1,23,456");
        expect(enIn.format(1234567)).toBe("12,34,567");

        const ar = new Intl.NumberFormat("ar", { useGrouping: "min2" });
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
        expect(ar.format(1234)).toBe("\u0661\u0662\u0663\u0664");
        expect(ar.format(12345)).toBe("\u0661\u0662\u066c\u0663\u0664\u0665");
        expect(ar.format(123456)).toBe("\u0661\u0662\u0663\u066c\u0664\u0665\u0666");
        expect(ar.format(1234567)).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0666\u0667");

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: "min2" });
        expect(plPl.format(123)).toBe("123");
        expect(plPl.format(1234)).toBe("1234");
        expect(plPl.format(12345)).toBe("12\u00a0345");
        expect(plPl.format(123456)).toBe("123\u00a0456");
        expect(plPl.format(1234567)).toBe("1\u00a0234\u00a0567");
    });

    test("useGrouping=false", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: false });
        expect(en.format(123)).toBe("123");
        expect(en.format(1234)).toBe("1234");
        expect(en.format(12345)).toBe("12345");
        expect(en.format(123456)).toBe("123456");
        expect(en.format(1234567)).toBe("1234567");

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: false });
        expect(enIn.format(123)).toBe("123");
        expect(enIn.format(1234)).toBe("1234");
        expect(enIn.format(12345)).toBe("12345");
        expect(enIn.format(123456)).toBe("123456");
        expect(enIn.format(1234567)).toBe("1234567");

        const ar = new Intl.NumberFormat("ar", { useGrouping: false });
        expect(ar.format(123)).toBe("\u0661\u0662\u0663");
        expect(ar.format(1234)).toBe("\u0661\u0662\u0663\u0664");
        expect(ar.format(12345)).toBe("\u0661\u0662\u0663\u0664\u0665");
        expect(ar.format(123456)).toBe("\u0661\u0662\u0663\u0664\u0665\u0666");
        expect(ar.format(1234567)).toBe("\u0661\u0662\u0663\u0664\u0665\u0666\u0667");

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: false });
        expect(plPl.format(123)).toBe("123");
        expect(plPl.format(1234)).toBe("1234");
        expect(plPl.format(12345)).toBe("12345");
        expect(plPl.format(123456)).toBe("123456");
        expect(plPl.format(1234567)).toBe("1234567");
    });

    test("roundingPriority=lessPrecision", () => {
        const nf = (locale, minSignificant, maxSignificant, minFraction, maxFraction) => {
            return new Intl.NumberFormat(locale, {
                roundingPriority: "lessPrecision",
                minimumSignificantDigits: minSignificant,
                maximumSignificantDigits: maxSignificant,
                minimumFractionDigits: minFraction,
                maximumFractionDigits: maxFraction,
            });
        };

        expect(nf("en", 2, undefined, 2, undefined).format(1)).toBe("1.00");
        expect(nf("en", 3, undefined, 1, undefined).format(1)).toBe("1.0");
        expect(nf("en", undefined, 2, undefined, 2).format(1.23)).toBe("1.2");
        expect(nf("en", undefined, 3, undefined, 1).format(1.23)).toBe("1.2");

        expect(nf("ar", 2, undefined, 2, undefined).format(1)).toBe("\u0661\u066b\u0660\u0660");
        expect(nf("ar", 3, undefined, 1, undefined).format(1)).toBe("\u0661\u066b\u0660");
        expect(nf("ar", undefined, 2, undefined, 2).format(1.23)).toBe("\u0661\u066b\u0662");
        expect(nf("ar", undefined, 3, undefined, 1).format(1.23)).toBe("\u0661\u066b\u0662");
    });

    test("roundingPriority=morePrecision", () => {
        const nf = (locale, minSignificant, maxSignificant, minFraction, maxFraction) => {
            return new Intl.NumberFormat(locale, {
                roundingPriority: "morePrecision",
                minimumSignificantDigits: minSignificant,
                maximumSignificantDigits: maxSignificant,
                minimumFractionDigits: minFraction,
                maximumFractionDigits: maxFraction,
            });
        };

        expect(nf("en", 2, undefined, 2, undefined).format(1)).toBe("1.0");
        expect(nf("en", 3, undefined, 1, undefined).format(1)).toBe("1.00");
        expect(nf("en", undefined, 2, undefined, 2).format(1.23)).toBe("1.23");
        expect(nf("en", undefined, 3, undefined, 1).format(1.23)).toBe("1.23");

        expect(nf("ar", 2, undefined, 2, undefined).format(1)).toBe("\u0661\u066b\u0660");
        expect(nf("ar", 3, undefined, 1, undefined).format(1)).toBe("\u0661\u066b\u0660\u0660");
        expect(nf("ar", undefined, 2, undefined, 2).format(1.23)).toBe("\u0661\u066b\u0662\u0663");
        expect(nf("ar", undefined, 3, undefined, 1).format(1.23)).toBe("\u0661\u066b\u0662\u0663");
    });

    test("roundingMode=ceil", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "ceil",
        });
        expect(en.format(1.11)).toBe("1.2");
        expect(en.format(1.15)).toBe("1.2");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.1");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "ceil",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=expand", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "expand",
        });
        expect(en.format(1.11)).toBe("1.2");
        expect(en.format(1.15)).toBe("1.2");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.2");
        expect(en.format(-1.15)).toBe("-1.2");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "expand",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=floor", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "floor",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.1");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.2");
        expect(en.format(-1.15)).toBe("-1.2");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "floor",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=halfCeil", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "halfCeil",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.2");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.1");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "halfCeil",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=halfEven", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "halfEven",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.2");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.2");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "halfEven",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=halfExpand", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "halfExpand",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.2");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.2");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "halfExpand",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=halfFloor", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "halfFloor",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.1");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.2");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "halfFloor",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0662");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=halfTrunc", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "halfTrunc",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.1");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.1");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "halfTrunc",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingMode=trunc", () => {
        const en = new Intl.NumberFormat("en", {
            maximumSignificantDigits: 2,
            roundingMode: "trunc",
        });
        expect(en.format(1.11)).toBe("1.1");
        expect(en.format(1.15)).toBe("1.1");
        expect(en.format(1.2)).toBe("1.2");
        expect(en.format(-1.11)).toBe("-1.1");
        expect(en.format(-1.15)).toBe("-1.1");
        expect(en.format(-1.2)).toBe("-1.2");

        const ar = new Intl.NumberFormat("ar", {
            maximumSignificantDigits: 2,
            roundingMode: "trunc",
        });
        expect(ar.format(1.11)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.15)).toBe("\u0661\u066b\u0661");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662");
        expect(ar.format(-1.11)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.15)).toBe("\u061c-\u0661\u066b\u0661");
        expect(ar.format(-1.2)).toBe("\u061c-\u0661\u066b\u0662");
    });

    test("roundingIncrement", () => {
        const nf = (roundingIncrement, fractionDigits) => {
            return new Intl.NumberFormat([], {
                roundingIncrement: roundingIncrement,
                minimumFractionDigits: fractionDigits,
                maximumFractionDigits: fractionDigits,
            });
        };

        const nf1 = nf(1, 2);
        expect(nf1.format(1.01)).toBe("1.01");
        expect(nf1.format(1.02)).toBe("1.02");
        expect(nf1.format(1.03)).toBe("1.03");
        expect(nf1.format(1.04)).toBe("1.04");
        expect(nf1.format(1.05)).toBe("1.05");

        const nf2 = nf(2, 2);
        expect(nf2.format(1.01)).toBe("1.02");
        expect(nf2.format(1.02)).toBe("1.02");
        expect(nf2.format(1.03)).toBe("1.04");
        expect(nf2.format(1.04)).toBe("1.04");
        expect(nf2.format(1.05)).toBe("1.06");

        const nf5 = nf(5, 2);
        expect(nf5.format(1.01)).toBe("1.00");
        expect(nf5.format(1.02)).toBe("1.00");
        expect(nf5.format(1.03)).toBe("1.05");
        expect(nf5.format(1.04)).toBe("1.05");
        expect(nf5.format(1.05)).toBe("1.05");

        const nf10 = nf(10, 2);
        expect(nf10.format(1.1)).toBe("1.10");
        expect(nf10.format(1.12)).toBe("1.10");
        expect(nf10.format(1.15)).toBe("1.20");
        expect(nf10.format(1.2)).toBe("1.20");

        const nf20 = nf(20, 2);
        expect(nf20.format(1.05)).toBe("1.00");
        expect(nf20.format(1.1)).toBe("1.20");
        expect(nf20.format(1.15)).toBe("1.20");
        expect(nf20.format(1.2)).toBe("1.20");

        const nf25 = nf(25, 2);
        expect(nf25.format(1.25)).toBe("1.25");
        expect(nf25.format(1.3125)).toBe("1.25");
        expect(nf25.format(1.375)).toBe("1.50");
        expect(nf25.format(1.5)).toBe("1.50");

        const nf50 = nf(50, 2);
        expect(nf50.format(1.5)).toBe("1.50");
        expect(nf50.format(1.625)).toBe("1.50");
        expect(nf50.format(1.75)).toBe("2.00");
        expect(nf50.format(1.875)).toBe("2.00");
        expect(nf50.format(2.0)).toBe("2.00");

        const nf100 = nf(100, 3);
        expect(nf100.format(1.1)).toBe("1.100");
        expect(nf100.format(1.125)).toBe("1.100");
        expect(nf100.format(1.15)).toBe("1.200");
        expect(nf100.format(1.175)).toBe("1.200");
        expect(nf100.format(1.2)).toBe("1.200");

        const nf200 = nf(200, 3);
        expect(nf200.format(1.2)).toBe("1.200");
        expect(nf200.format(1.25)).toBe("1.200");
        expect(nf200.format(1.3)).toBe("1.400");
        expect(nf200.format(1.35)).toBe("1.400");
        expect(nf200.format(1.4)).toBe("1.400");

        const nf250 = nf(250, 3);
        expect(nf250.format(1.25)).toBe("1.250");
        expect(nf250.format(1.3125)).toBe("1.250");
        expect(nf250.format(1.375)).toBe("1.500");
        expect(nf250.format(1.4375)).toBe("1.500");
        expect(nf250.format(1.5)).toBe("1.500");

        const nf500 = nf(500, 3);
        expect(nf500.format(1.5)).toBe("1.500");
        expect(nf500.format(1.625)).toBe("1.500");
        expect(nf500.format(1.75)).toBe("2.000");
        expect(nf500.format(1.875)).toBe("2.000");
        expect(nf500.format(2.0)).toBe("2.000");

        const nf1000 = nf(1000, 4);
        expect(nf1000.format(1.1)).toBe("1.1000");
        expect(nf1000.format(1.125)).toBe("1.1000");
        expect(nf1000.format(1.15)).toBe("1.2000");
        expect(nf1000.format(1.175)).toBe("1.2000");
        expect(nf1000.format(1.2)).toBe("1.2000");

        const nf2000 = nf(2000, 4);
        expect(nf2000.format(1.2)).toBe("1.2000");
        expect(nf2000.format(1.25)).toBe("1.2000");
        expect(nf2000.format(1.3)).toBe("1.4000");
        expect(nf2000.format(1.35)).toBe("1.4000");
        expect(nf2000.format(1.4)).toBe("1.4000");

        const nf2500 = nf(2500, 4);
        expect(nf2500.format(1.25)).toBe("1.2500");
        expect(nf2500.format(1.3125)).toBe("1.2500");
        expect(nf2500.format(1.375)).toBe("1.5000");
        expect(nf2500.format(1.4375)).toBe("1.5000");
        expect(nf2500.format(1.5)).toBe("1.5000");

        const nf5000 = nf(5000, 4);
        expect(nf5000.format(1.5)).toBe("1.5000");
        expect(nf5000.format(1.625)).toBe("1.5000");
        expect(nf5000.format(1.75)).toBe("2.0000");
        expect(nf5000.format(1.875)).toBe("2.0000");
        expect(nf5000.format(2.0)).toBe("2.0000");
    });

    test("trailingZeroDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", {
            trailingZeroDisplay: "auto",
            minimumSignificantDigits: 5,
        });
        expect(en.format(1)).toBe("1.0000");
        expect(en.format(1n)).toBe("1.0000");
        expect(en.format(12)).toBe("12.000");
        expect(en.format(12n)).toBe("12.000");
        expect(en.format(1.2)).toBe("1.2000");

        const ar = new Intl.NumberFormat("ar", {
            trailingZeroDisplay: "auto",
            minimumSignificantDigits: 5,
        });
        expect(ar.format(1)).toBe("\u0661\u066b\u0660\u0660\u0660\u0660");
        expect(ar.format(1n)).toBe("\u0661\u066b\u0660\u0660\u0660\u0660");
        expect(ar.format(12)).toBe("\u0661\u0662\u066b\u0660\u0660\u0660");
        expect(ar.format(12n)).toBe("\u0661\u0662\u066b\u0660\u0660\u0660");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662\u0660\u0660\u0660");
    });

    test("trailingZeroDisplay=stripIfInteger", () => {
        const en = new Intl.NumberFormat("en", {
            trailingZeroDisplay: "stripIfInteger",
            minimumSignificantDigits: 5,
        });
        expect(en.format(1)).toBe("1");
        expect(en.format(1n)).toBe("1");
        expect(en.format(12)).toBe("12");
        expect(en.format(12n)).toBe("12");
        expect(en.format(1.2)).toBe("1.2000");

        const ar = new Intl.NumberFormat("ar", {
            trailingZeroDisplay: "stripIfInteger",
            minimumSignificantDigits: 5,
        });
        expect(ar.format(1)).toBe("\u0661");
        expect(ar.format(1n)).toBe("\u0661");
        expect(ar.format(12)).toBe("\u0661\u0662");
        expect(ar.format(12n)).toBe("\u0661\u0662");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662\u0660\u0660\u0660");
    });
});

describe("style=percent", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en", { style: "percent" });
        expect(en.format(1)).toBe("100%");
        expect(en.format(1.2)).toBe("120%");
        expect(en.format(0.234)).toBe("23%");

        const ar = new Intl.NumberFormat("ar", { style: "percent" });
        expect(ar.format(1)).toBe("\u0661\u0660\u0660\u066a\u061c");
        expect(ar.format(1.2)).toBe("\u0661\u0662\u0660\u066a\u061c");
        expect(ar.format(0.234)).toBe("\u0662\u0663\u066a\u061c");
    });

    test("integer digits", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", minimumIntegerDigits: 2 });
        expect(en.format(0.01)).toBe("01%");
        expect(en.format(0.12)).toBe("12%");
        expect(en.format(1.23)).toBe("123%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", minimumIntegerDigits: 2 });
        expect(ar.format(0.01)).toBe("\u0660\u0661\u066a\u061c");
        expect(ar.format(0.12)).toBe("\u0661\u0662\u066a\u061c");
        expect(ar.format(1.23)).toBe("\u0661\u0662\u0663\u066a\u061c");
    });

    test("significant digits", () => {
        const en = new Intl.NumberFormat("en", {
            style: "percent",
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(en.format(0.1)).toBe("10.00%");
        expect(en.format(1.2)).toBe("120.0%");
        expect(en.format(1.23)).toBe("123.0%");
        expect(en.format(1.234)).toBe("123.4%");
        expect(en.format(1.2345)).toBe("123.45%");
        expect(en.format(1.23456)).toBe("123.456%");
        expect(en.format(1.234567)).toBe("123.457%");
        expect(en.format(1.234561)).toBe("123.456%");

        const ar = new Intl.NumberFormat("ar", {
            style: "percent",
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(ar.format(0.1)).toBe("\u0661\u0660\u066b\u0660\u0660\u066a\u061c");
        expect(ar.format(1.2)).toBe("\u0661\u0662\u0660\u066b\u0660\u066a\u061c");
        expect(ar.format(1.23)).toBe("\u0661\u0662\u0663\u066b\u0660\u066a\u061c");
        expect(ar.format(1.234)).toBe("\u0661\u0662\u0663\u066b\u0664\u066a\u061c");
        expect(ar.format(1.2345)).toBe("\u0661\u0662\u0663\u066b\u0664\u0665\u066a\u061c");
        expect(ar.format(1.23456)).toBe("\u0661\u0662\u0663\u066b\u0664\u0665\u0666\u066a\u061c");
        expect(ar.format(1.234567)).toBe("\u0661\u0662\u0663\u066b\u0664\u0665\u0667\u066a\u061c");
        expect(ar.format(1.234561)).toBe("\u0661\u0662\u0663\u066b\u0664\u0665\u0666\u066a\u061c");
    });

    test("fraction digits", () => {
        const en = new Intl.NumberFormat("en", {
            style: "percent",
            minimumFractionDigits: 2,
            maximumFractionDigits: 4,
        });
        expect(en.format(0.01)).toBe("1.00%");
        expect(en.format(0.1)).toBe("10.00%");
        expect(en.format(0.12)).toBe("12.00%");
        expect(en.format(0.123)).toBe("12.30%");
        expect(en.format(0.1234)).toBe("12.34%");
        expect(en.format(0.12345)).toBe("12.345%");
        expect(en.format(0.123456)).toBe("12.3456%");
        expect(en.format(0.1234567)).toBe("12.3457%");
        expect(en.format(0.1234561)).toBe("12.3456%");

        const ar = new Intl.NumberFormat("ar", {
            style: "percent",
            minimumFractionDigits: 2,
            maximumFractionDigits: 4,
        });
        expect(ar.format(0.01)).toBe("\u0661\u066b\u0660\u0660\u066a\u061c");
        expect(ar.format(0.1)).toBe("\u0661\u0660\u066b\u0660\u0660\u066a\u061c");
        expect(ar.format(0.12)).toBe("\u0661\u0662\u066b\u0660\u0660\u066a\u061c");
        expect(ar.format(0.123)).toBe("\u0661\u0662\u066b\u0663\u0660\u066a\u061c");
        expect(ar.format(0.1234)).toBe("\u0661\u0662\u066b\u0663\u0664\u066a\u061c");
        expect(ar.format(0.12345)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u066a\u061c");
        expect(ar.format(0.123456)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0666\u066a\u061c");
        expect(ar.format(0.1234567)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0667\u066a\u061c");
        expect(ar.format(0.1234561)).toBe("\u0661\u0662\u066b\u0663\u0664\u0665\u0666\u066a\u061c");
    });

    test("notation=compact", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", notation: "compact" });
        expect(en.format(0.01)).toBe("1%");
        expect(en.format(0.012)).toBe("1.2%");
        expect(en.format(0.0123)).toBe("1.2%");
        expect(en.format(0.0129)).toBe("1.3%");
        expect(en.format(0.12)).toBe("12%");
        expect(en.format(0.123)).toBe("12%");
        expect(en.format(0.1234)).toBe("12%");

        const enFullwide = new Intl.NumberFormat("en", {
            style: "percent",
            notation: "compact",
            numberingSystem: "fullwide",
        });
        expect(enFullwide.format(0.01)).toBe("１%");
        expect(enFullwide.format(0.012)).toBe("１.２%");
        expect(enFullwide.format(0.0123)).toBe("１.２%");
        expect(enFullwide.format(0.0129)).toBe("１.３%");
        expect(enFullwide.format(0.12)).toBe("１２%");
        expect(enFullwide.format(0.123)).toBe("１２%");
        expect(enFullwide.format(0.1234)).toBe("１２%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", notation: "compact" });
        expect(ar.format(0.01)).toBe("\u0661\u066a");
        expect(ar.format(0.012)).toBe("\u0661\u066b\u0662\u066a");
        expect(ar.format(0.0123)).toBe("\u0661\u066b\u0662\u066a");
        expect(ar.format(0.0129)).toBe("\u0661\u066b\u0663\u066a");
        expect(ar.format(0.12)).toBe("\u0661\u0662\u066a");
        expect(ar.format(0.123)).toBe("\u0661\u0662\u066a");
        expect(ar.format(0.1234)).toBe("\u0661\u0662\u066a");
    });

    test("signDisplay=never", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "never" });
        expect(en.format(0.01)).toBe("1%");
        expect(en.format(-0.01)).toBe("1%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "never" });
        expect(ar.format(0.01)).toBe("\u0661\u066a\u061c");
        expect(ar.format(-0.01)).toBe("\u0661\u066a\u061c");
    });

    test("signDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "auto" });
        expect(en.format(0.0)).toBe("0%");
        expect(en.format(0.01)).toBe("1%");
        expect(en.format(-0.0)).toBe("-0%");
        expect(en.format(-0.01)).toBe("-1%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "auto" });
        expect(ar.format(0.0)).toBe("\u0660\u066a\u061c");
        expect(ar.format(0.01)).toBe("\u0661\u066a\u061c");
        expect(ar.format(-0.0)).toBe("\u061c-\u0660\u066a\u061c");
        expect(ar.format(-0.01)).toBe("\u061c-\u0661\u066a\u061c");
    });

    test("signDisplay=always", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "always" });
        expect(en.format(0.0)).toBe("+0%");
        expect(en.format(0.01)).toBe("+1%");
        expect(en.format(-0.0)).toBe("-0%");
        expect(en.format(-0.01)).toBe("-1%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "always" });
        expect(ar.format(0.0)).toBe("\u061c+\u0660\u066a\u061c");
        expect(ar.format(0.01)).toBe("\u061c+\u0661\u066a\u061c");
        expect(ar.format(-0.0)).toBe("\u061c-\u0660\u066a\u061c");
        expect(ar.format(-0.01)).toBe("\u061c-\u0661\u066a\u061c");
    });

    test("signDisplay=exceptZero", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "exceptZero" });
        expect(en.format(0.0)).toBe("0%");
        expect(en.format(0.01)).toBe("+1%");
        expect(en.format(-0.0)).toBe("0%");
        expect(en.format(-0.01)).toBe("-1%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "exceptZero" });
        expect(ar.format(0.0)).toBe("\u0660\u066a\u061c");
        expect(ar.format(0.01)).toBe("\u061c+\u0661\u066a\u061c");
        expect(ar.format(-0.0)).toBe("\u0660\u066a\u061c");
        expect(ar.format(-0.01)).toBe("\u061c-\u0661\u066a\u061c");
    });

    test("signDisplay=negative", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "negative" });
        expect(en.format(0.0)).toBe("0%");
        expect(en.format(0.01)).toBe("1%");
        expect(en.format(-0.0)).toBe("0%");
        expect(en.format(-0.01)).toBe("-1%");

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "negative" });
        expect(ar.format(0.0)).toBe("\u0660\u066a\u061c");
        expect(ar.format(0.01)).toBe("\u0661\u066a\u061c");
        expect(ar.format(-0.0)).toBe("\u0660\u066a\u061c");
        expect(ar.format(-0.01)).toBe("\u061c-\u0661\u066a\u061c");
    });
});

describe("style=currency", () => {
    test("currencyDisplay=code", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "code",
        });
        expect(en1.format(1)).toBe("USD\u00a01.00");
        expect(en1.format(1.2)).toBe("USD\u00a01.20");
        expect(en1.format(1.23)).toBe("USD\u00a01.23");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "KHR",
            currencyDisplay: "code",
        });
        expect(en2.format(1)).toBe("KHR\u00a01.00");
        expect(en2.format(1.2)).toBe("KHR\u00a01.20");
        expect(en2.format(1.23)).toBe("KHR\u00a01.23");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "code",
        });
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0USD");
        expect(ar1.format(1.2)).toBe("\u200f\u0661\u066b\u0662\u0660\u00a0USD");
        expect(ar1.format(1.23)).toBe("\u200f\u0661\u066b\u0662\u0663\u00a0USD");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "code",
            numberingSystem: "latn",
        });
        expect(ar2.format(1)).toBe("\u200f1.00\u00a0USD");
        expect(ar2.format(1.2)).toBe("\u200f1.20\u00a0USD");
        expect(ar2.format(1.23)).toBe("\u200f1.23\u00a0USD");
    });

    test("currencyDisplay=symbol", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "symbol",
        });
        expect(en1.format(1)).toBe("$1.00");
        expect(en1.format(1.2)).toBe("$1.20");
        expect(en1.format(1.23)).toBe("$1.23");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "KHR",
            currencyDisplay: "symbol",
        });
        expect(en2.format(1)).toBe("KHR\u00a01.00");
        expect(en2.format(1.2)).toBe("KHR\u00a01.20");
        expect(en2.format(1.23)).toBe("KHR\u00a01.23");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "symbol",
        });
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1.2)).toBe("\u200f\u0661\u066b\u0662\u0660\u00a0US$");
        expect(ar1.format(1.23)).toBe("\u200f\u0661\u066b\u0662\u0663\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "symbol",
            numberingSystem: "latn",
        });
        expect(ar2.format(1)).toBe("\u200f1.00\u00a0US$");
        expect(ar2.format(1.2)).toBe("\u200f1.20\u00a0US$");
        expect(ar2.format(1.23)).toBe("\u200f1.23\u00a0US$");
    });

    test("currencyDisplay=narrowSymbol", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "narrowSymbol",
        });
        expect(en1.format(1)).toBe("$1.00");
        expect(en1.format(1.2)).toBe("$1.20");
        expect(en1.format(1.23)).toBe("$1.23");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "KHR",
            currencyDisplay: "narrowSymbol",
        });
        expect(en2.format(1)).toBe("៛1.00");
        expect(en2.format(1.2)).toBe("៛1.20");
        expect(en2.format(1.23)).toBe("៛1.23");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "narrowSymbol",
        });
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1.2)).toBe("\u200f\u0661\u066b\u0662\u0660\u00a0US$");
        expect(ar1.format(1.23)).toBe("\u200f\u0661\u066b\u0662\u0663\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "narrowSymbol",
            numberingSystem: "latn",
        });
        expect(ar2.format(1)).toBe("\u200f1.00\u00a0US$");
        expect(ar2.format(1.2)).toBe("\u200f1.20\u00a0US$");
        expect(ar2.format(1.23)).toBe("\u200f1.23\u00a0US$");
    });

    test("currencyDisplay=name", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "name",
        });
        expect(en1.format(1)).toBe("1.00 US dollars");
        expect(en1.format(1.2)).toBe("1.20 US dollars");
        expect(en1.format(1.23)).toBe("1.23 US dollars");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "KHR",
            currencyDisplay: "name",
        });
        expect(en2.format(1)).toBe("1.00 Cambodian riels");
        expect(en2.format(1.2)).toBe("1.20 Cambodian riels");
        expect(en2.format(1.23)).toBe("1.23 Cambodian riels");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "name",
        });
        expect(ar1.format(1)).toBe("\u0661\u066b\u0660\u0660 دولار أمريكي");
        expect(ar1.format(1.2)).toBe("\u0661\u066b\u0662\u0660 دولار أمريكي");
        expect(ar1.format(1.23)).toBe("\u0661\u066b\u0662\u0663 دولار أمريكي");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "name",
            numberingSystem: "latn",
        });
        expect(ar2.format(1)).toBe("1.00 دولار أمريكي");
        expect(ar2.format(1.2)).toBe("1.20 دولار أمريكي");
        expect(ar2.format(1.23)).toBe("1.23 دولار أمريكي");
    });

    test("notation=compact, compactDisplay=long", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            notation: "compact",
            compactDisplay: "long",
        });
        expect(en.format(1)).toBe("$1");
        expect(en.format(1200)).toBe("$1.2K");
        expect(en.format(1290)).toBe("$1.3K");
        expect(en.format(12000)).toBe("$12K");
        expect(en.format(12900)).toBe("$13K");
        expect(en.format(1200000)).toBe("$1.2M");
        expect(en.format(1290000)).toBe("$1.3M");
        expect(en.format(12000000)).toBe("$12M");
        expect(en.format(12900000)).toBe("$13M");

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            notation: "compact",
            compactDisplay: "long",
        });
        expect(ar.format(1)).toBe("‏١ US$");
        expect(ar.format(1200)).toBe("١٫٢ ألف US$");
        expect(ar.format(1290)).toBe("١٫٣ ألف US$");
        expect(ar.format(12000)).toBe("١٢ ألف US$");
        expect(ar.format(12900)).toBe("١٣ ألف US$");
        expect(ar.format(1200000)).toBe("١٫٢ مليون US$");
        expect(ar.format(1290000)).toBe("١٫٣ مليون US$");
        expect(ar.format(12000000)).toBe("١٢ مليون US$");
        expect(ar.format(12900000)).toBe("١٣ مليون US$");

        const ja = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            notation: "compact",
            compactDisplay: "long",
        });
        expect(ja.format(1)).toBe("￥1");
        expect(ja.format(1200)).toBe("￥1200");
        expect(ja.format(1290)).toBe("￥1290");
        expect(ja.format(12000)).toBe("￥1.2万");
        expect(ja.format(12900)).toBe("￥1.3万");
        expect(ja.format(1200000)).toBe("￥120万");
        expect(ja.format(1290000)).toBe("￥129万");
        expect(ja.format(12000000)).toBe("￥1200万");
        expect(ja.format(12900000)).toBe("￥1290万");
        expect(ja.format(120000000)).toBe("￥1.2億");
        expect(ja.format(129000000)).toBe("￥1.3億");
        expect(ja.format(12000000000)).toBe("￥120億");
        expect(ja.format(12900000000)).toBe("￥129億");

        const de = new Intl.NumberFormat("de", {
            style: "currency",
            currency: "EUR",
            notation: "compact",
            compactDisplay: "long",
        });
        expect(de.format(1)).toBe("1 €");
        expect(de.format(1200)).toBe("1200 €");
        expect(de.format(1290)).toBe("1290 €");
        expect(de.format(12000)).toBe("12.000 €");
        expect(de.format(12900)).toBe("12.900 €");
        expect(de.format(1200000)).toBe("1,2 Mio. €");
        expect(de.format(1290000)).toBe("1,3 Mio. €");
        expect(de.format(12000000)).toBe("12 Mio. €");
        expect(de.format(12900000)).toBe("13 Mio. €");
    });

    test("notation=compact, compactDisplay=short", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            notation: "compact",
            compactDisplay: "short",
        });
        expect(en.format(1)).toBe("$1");
        expect(en.format(1200)).toBe("$1.2K");
        expect(en.format(1290)).toBe("$1.3K");
        expect(en.format(12000)).toBe("$12K");
        expect(en.format(12900)).toBe("$13K");
        expect(en.format(1200000)).toBe("$1.2M");
        expect(en.format(1290000)).toBe("$1.3M");
        expect(en.format(12000000)).toBe("$12M");
        expect(en.format(12900000)).toBe("$13M");

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            notation: "compact",
            compactDisplay: "short",
        });
        expect(ar.format(1)).toBe("‏١ US$");
        expect(ar.format(1200)).toBe("١٫٢ ألف US$");
        expect(ar.format(1290)).toBe("١٫٣ ألف US$");
        expect(ar.format(12000)).toBe("١٢ ألف US$");
        expect(ar.format(12900)).toBe("١٣ ألف US$");
        expect(ar.format(1200000)).toBe("١٫٢ مليون US$");
        expect(ar.format(1290000)).toBe("١٫٣ مليون US$");
        expect(ar.format(12000000)).toBe("١٢ مليون US$");
        expect(ar.format(12900000)).toBe("١٣ مليون US$");

        const ja = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            notation: "compact",
            compactDisplay: "short",
        });
        expect(ja.format(1)).toBe("￥1");
        expect(ja.format(1200)).toBe("￥1200");
        expect(ja.format(1290)).toBe("￥1290");
        expect(ja.format(12000)).toBe("￥1.2万");
        expect(ja.format(12900)).toBe("￥1.3万");
        expect(ja.format(1200000)).toBe("￥120万");
        expect(ja.format(1290000)).toBe("￥129万");
        expect(ja.format(12000000)).toBe("￥1200万");
        expect(ja.format(12900000)).toBe("￥1290万");
        expect(ja.format(120000000)).toBe("￥1.2億");
        expect(ja.format(129000000)).toBe("￥1.3億");
        expect(ja.format(12000000000)).toBe("￥120億");
        expect(ja.format(12900000000)).toBe("￥129億");

        const de = new Intl.NumberFormat("de", {
            style: "currency",
            currency: "EUR",
            notation: "compact",
            compactDisplay: "short",
        });
        expect(de.format(1)).toBe("1 €");
        expect(de.format(1200)).toBe("1200 €");
        expect(de.format(1290)).toBe("1290 €");
        expect(de.format(12000)).toBe("12.000 €");
        expect(de.format(12900)).toBe("12.900 €");
        expect(de.format(1200000)).toBe("1,2 Mio. €");
        expect(de.format(1290000)).toBe("1,3 Mio. €");
        expect(de.format(12000000)).toBe("12 Mio. €");
        expect(de.format(12900000)).toBe("13 Mio. €");
    });

    test("signDisplay=never", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "never",
        });
        expect(en1.format(1)).toBe("$1.00");
        expect(en1.format(-1)).toBe("$1.00");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "never",
        });
        expect(en2.format(1)).toBe("$1.00");
        expect(en2.format(-1)).toBe("$1.00");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "never",
        });
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "never",
        });
        expect(ar2.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
    });

    test("signDisplay=auto", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "auto",
        });
        expect(en1.format(0)).toBe("$0.00");
        expect(en1.format(1)).toBe("$1.00");
        expect(en1.format(-0)).toBe("-$0.00");
        expect(en1.format(-1)).toBe("-$1.00");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "auto",
        });
        expect(en2.format(0)).toBe("$0.00");
        expect(en2.format(1)).toBe("$1.00");
        expect(en2.format(-0)).toBe("($0.00)");
        expect(en2.format(-1)).toBe("($1.00)");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "auto",
        });
        expect(ar1.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-0)).toBe("\u061c-\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "auto",
        });
        expect(ar2.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-0)).toBe("\u061c-\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");
    });

    test("signDisplay=always", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "always",
        });
        expect(en1.format(0)).toBe("+$0.00");
        expect(en1.format(1)).toBe("+$1.00");
        expect(en1.format(-0)).toBe("-$0.00");
        expect(en1.format(-1)).toBe("-$1.00");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "always",
        });
        expect(en2.format(0)).toBe("+$0.00");
        expect(en2.format(1)).toBe("+$1.00");
        expect(en2.format(-0)).toBe("($0.00)");
        expect(en2.format(-1)).toBe("($1.00)");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "always",
        });
        expect(ar1.format(0)).toBe("\u061c+\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1)).toBe("\u061c+\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-0)).toBe("\u061c-\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "always",
        });
        expect(ar2.format(0)).toBe("\u061c+\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(1)).toBe("\u061c+\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-0)).toBe("\u061c-\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");
    });

    test("signDisplay=exceptZero", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "exceptZero",
        });
        expect(en1.format(0)).toBe("$0.00");
        expect(en1.format(1)).toBe("+$1.00");
        expect(en1.format(-0)).toBe("$0.00");
        expect(en1.format(-1)).toBe("-$1.00");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "exceptZero",
        });
        expect(en2.format(0)).toBe("$0.00");
        expect(en2.format(1)).toBe("+$1.00");
        expect(en2.format(-0)).toBe("$0.00");
        expect(en2.format(-1)).toBe("($1.00)");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "exceptZero",
        });
        expect(ar1.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1)).toBe("\u061c+\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "exceptZero",
        });
        expect(ar2.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(1)).toBe("\u061c+\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");
    });

    test("signDisplay=negative", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "negative",
        });
        expect(en1.format(0)).toBe("$0.00");
        expect(en1.format(1)).toBe("$1.00");
        expect(en1.format(-0)).toBe("$0.00");
        expect(en1.format(-1)).toBe("-$1.00");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "negative",
        });
        expect(en2.format(0)).toBe("$0.00");
        expect(en2.format(1)).toBe("$1.00");
        expect(en2.format(-0)).toBe("$0.00");
        expect(en2.format(-1)).toBe("($1.00)");

        const ar1 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "negative",
        });
        expect(ar1.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar1.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");

        const ar2 = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "negative",
        });
        expect(ar2.format(0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(1)).toBe("\u200f\u0661\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-0)).toBe("\u200f\u0660\u066b\u0660\u0660\u00a0US$");
        expect(ar2.format(-1)).toBe("\u061c-\u200f\u0661\u066b\u0660\u0660\u00a0US$");
    });
});

describe("style=unit", () => {
    test("unitDisplay=long", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "long",
        });
        expect(en1.format(1)).toBe("1 gigabit");
        expect(en1.format(1.2)).toBe("1.2 gigabits");
        expect(en1.format(123)).toBe("123 gigabits");

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "long",
        });
        expect(en2.format(1)).toBe("1 kilometer per hour");
        expect(en2.format(1.2)).toBe("1.2 kilometers per hour");
        expect(en2.format(123)).toBe("123 kilometers per hour");

        const en3 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "nanosecond",
            unitDisplay: "long",
        });
        expect(en3.format(1)).toBe("1 nanosecond");
        expect(en3.format(1.2)).toBe("1.2 nanoseconds");
        expect(en3.format(123)).toBe("123 nanoseconds");

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "long",
        });
        expect(ar.format(1)).toBe("قدم");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662 قدم");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663 قدم");

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "long",
        });
        expect(ja.format(1)).toBe("時速 1 キロメートル");
        expect(ja.format(1.2)).toBe("時速 1.2 キロメートル");
        expect(ja.format(123)).toBe("時速 123 キロメートル");
    });

    test("unitDisplay=short", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "short",
        });
        expect(en1.format(1)).toBe("1 Gb");
        expect(en1.format(1.2)).toBe("1.2 Gb");
        expect(en1.format(123)).toBe("123 Gb");

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "short",
        });
        expect(en2.format(1)).toBe("1 km/h");
        expect(en2.format(1.2)).toBe("1.2 km/h");
        expect(en2.format(123)).toBe("123 km/h");

        const en3 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "nanosecond",
            unitDisplay: "short",
        });
        expect(en3.format(1)).toBe("1 ns");
        expect(en3.format(1.2)).toBe("1.2 ns");
        expect(en3.format(123)).toBe("123 ns");

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "short",
        });
        expect(ar.format(1)).toBe("قدم");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662 قدم");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663 قدم");

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "short",
        });
        expect(ja.format(1)).toBe("1 km/h");
        expect(ja.format(1.2)).toBe("1.2 km/h");
        expect(ja.format(123)).toBe("123 km/h");
    });

    test("unitDisplay=narrow", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "narrow",
        });
        expect(en1.format(1)).toBe("1Gb");
        expect(en1.format(1.2)).toBe("1.2Gb");
        expect(en1.format(123)).toBe("123Gb");

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "narrow",
        });
        expect(en2.format(1)).toBe("1km/h");
        expect(en2.format(1.2)).toBe("1.2km/h");
        expect(en2.format(123)).toBe("123km/h");

        const en3 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "nanosecond",
            unitDisplay: "narrow",
        });
        expect(en3.format(1)).toBe("1ns");
        expect(en3.format(1.2)).toBe("1.2ns");
        expect(en3.format(123)).toBe("123ns");

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "narrow",
        });
        expect(ar.format(1)).toBe("قدم");
        expect(ar.format(1.2)).toBe("\u0661\u066b\u0662 قدم");
        expect(ar.format(123)).toBe("\u0661\u0662\u0663 قدمًا");

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "narrow",
        });
        expect(ja.format(1)).toBe("1km/h");
        expect(ja.format(1.2)).toBe("1.2km/h");
        expect(ja.format(123)).toBe("123km/h");
    });
});

describe("bigint", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.format(1n)).toBe("1");
        expect(en.format(12n)).toBe("12");
        expect(en.format(123n)).toBe("123");
        expect(en.format(123456789123456789123456789123456789n)).toBe(
            "123,456,789,123,456,789,123,456,789,123,456,789"
        );

        const ar = new Intl.NumberFormat("ar");
        expect(ar.format(1n)).toBe("\u0661");
        expect(ar.format(12n)).toBe("\u0661\u0662");
        expect(ar.format(123n)).toBe("\u0661\u0662\u0663");
        expect(ar.format(123456789123456789123456789123456789n)).toBe(
            "\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669"
        );
    });

    test("integer digits", () => {
        const en = new Intl.NumberFormat("en", { minimumIntegerDigits: 2 });
        expect(en.format(1n)).toBe("01");
        expect(en.format(12n)).toBe("12");
        expect(en.format(123n)).toBe("123");

        const ar = new Intl.NumberFormat("ar", { minimumIntegerDigits: 2 });
        expect(ar.format(1n)).toBe("\u0660\u0661");
        expect(ar.format(12n)).toBe("\u0661\u0662");
        expect(ar.format(123n)).toBe("\u0661\u0662\u0663");
    });

    test("significant digits", () => {
        const en = new Intl.NumberFormat("en", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(en.format(1n)).toBe("1.000");
        expect(en.format(12n)).toBe("12.00");
        expect(en.format(123n)).toBe("123.0");
        expect(en.format(1234n)).toBe("1,234");
        expect(en.format(12345n)).toBe("12,345");
        expect(en.format(123456n)).toBe("123,456");
        expect(en.format(1234567n)).toBe("1,234,570");
        expect(en.format(1234561n)).toBe("1,234,560");

        const ar = new Intl.NumberFormat("ar", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(ar.format(1n)).toBe("\u0661\u066b\u0660\u0660\u0660");
        expect(ar.format(12n)).toBe("\u0661\u0662\u066b\u0660\u0660");
        expect(ar.format(123n)).toBe("\u0661\u0662\u0663\u066b\u0660");
        expect(ar.format(1234n)).toBe("\u0661\u066c\u0662\u0663\u0664");
        expect(ar.format(12345n)).toBe("\u0661\u0662\u066c\u0663\u0664\u0665");
        expect(ar.format(123456n)).toBe("\u0661\u0662\u0663\u066c\u0664\u0665\u0666");
        expect(ar.format(1234567n)).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0667\u0660");
        expect(ar.format(1234561n)).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0666\u0660");
    });
});
