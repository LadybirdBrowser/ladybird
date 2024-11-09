describe("errors", () => {
    test("called on non-NumberFormat object", () => {
        expect(() => {
            Intl.NumberFormat.prototype.formatToParts(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.NumberFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            Intl.NumberFormat().formatToParts(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("special values", () => {
    test("NaN", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatToParts()).toEqual([{ type: "nan", value: "NaN" }]);
        expect(en.formatToParts(NaN)).toEqual([{ type: "nan", value: "NaN" }]);
        expect(en.formatToParts(undefined)).toEqual([{ type: "nan", value: "NaN" }]);

        const ar = new Intl.NumberFormat("ar");
        expect(ar.formatToParts()).toEqual([{ type: "nan", value: "ليس رقم" }]);
        expect(ar.formatToParts(NaN)).toEqual([{ type: "nan", value: "ليس رقم" }]);
        expect(ar.formatToParts(undefined)).toEqual([{ type: "nan", value: "ليس رقم" }]);
    });

    test("Infinity", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatToParts(Infinity)).toEqual([{ type: "infinity", value: "∞" }]);
        expect(en.formatToParts(-Infinity)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "infinity", value: "∞" },
        ]);

        const ar = new Intl.NumberFormat("ar");
        expect(ar.formatToParts(Infinity)).toEqual([{ type: "infinity", value: "∞" }]);
        expect(ar.formatToParts(-Infinity)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "infinity", value: "∞" },
        ]);
    });
});

describe("style=decimal", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatToParts(123)).toEqual([{ type: "integer", value: "123" }]);
        expect(en.formatToParts(1.23)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
        ]);
        expect(en.formatToParts(12.3)).toEqual([
            { type: "integer", value: "12" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "3" },
        ]);

        const ar = new Intl.NumberFormat("ar");
        expect(ar.formatToParts(123)).toEqual([{ type: "integer", value: "\u0661\u0662\u0663" }]);
        expect(ar.formatToParts(1.23)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
        ]);
        expect(ar.formatToParts(12.3)).toEqual([
            { type: "integer", value: "\u0661\u0662" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0663" },
        ]);
    });

    test("signDisplay=never", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "never" });
        expect(en.formatToParts(1)).toEqual([{ type: "integer", value: "1" }]);
        expect(en.formatToParts(-1)).toEqual([{ type: "integer", value: "1" }]);

        const ar = new Intl.NumberFormat("ar", { signDisplay: "never" });
        expect(ar.formatToParts(1)).toEqual([{ type: "integer", value: "\u0661" }]);
        expect(ar.formatToParts(-1)).toEqual([{ type: "integer", value: "\u0661" }]);
    });

    test("signDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "auto" });
        expect(en.formatToParts(0)).toEqual([{ type: "integer", value: "0" }]);
        expect(en.formatToParts(1)).toEqual([{ type: "integer", value: "1" }]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "0" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
        ]);

        const ar = new Intl.NumberFormat("ar", { signDisplay: "auto" });
        expect(ar.formatToParts(0)).toEqual([{ type: "integer", value: "\u0660" }]);
        expect(ar.formatToParts(1)).toEqual([{ type: "integer", value: "\u0661" }]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0660" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
        ]);
    });

    test("signDisplay=always", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "always" });
        expect(en.formatToParts(0)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "0" },
        ]);
        expect(en.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "1" },
        ]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "0" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
        ]);

        const ar = new Intl.NumberFormat("ar", { signDisplay: "always" });
        expect(ar.formatToParts(0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0660" },
        ]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0661" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0660" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
        ]);
    });

    test("signDisplay=exceptZero", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "exceptZero" });
        expect(en.formatToParts(0)).toEqual([{ type: "integer", value: "0" }]);
        expect(en.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "1" },
        ]);
        expect(en.formatToParts(-0)).toEqual([{ type: "integer", value: "0" }]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
        ]);

        const ar = new Intl.NumberFormat("ar", { signDisplay: "exceptZero" });
        expect(ar.formatToParts(0)).toEqual([{ type: "integer", value: "\u0660" }]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0661" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([{ type: "integer", value: "\u0660" }]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
        ]);
    });

    test("signDisplay=negative", () => {
        const en = new Intl.NumberFormat("en", { signDisplay: "negative" });
        expect(en.formatToParts(0)).toEqual([{ type: "integer", value: "0" }]);
        expect(en.formatToParts(1)).toEqual([{ type: "integer", value: "1" }]);
        expect(en.formatToParts(-0)).toEqual([{ type: "integer", value: "0" }]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
        ]);

        const ar = new Intl.NumberFormat("ar", { signDisplay: "negative" });
        expect(ar.formatToParts(0)).toEqual([{ type: "integer", value: "\u0660" }]);
        expect(ar.formatToParts(1)).toEqual([{ type: "integer", value: "\u0661" }]);
        expect(ar.formatToParts(-0)).toEqual([{ type: "integer", value: "\u0660" }]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
        ]);
    });

    test("useGrouping=always", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "always" });
        expect(en.formatToParts(1234)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "," },
            { type: "integer", value: "234" },
        ]);
        expect(en.formatToParts(12345)).toEqual([
            { type: "integer", value: "12" },
            { type: "group", value: "," },
            { type: "integer", value: "345" },
        ]);

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: "always" });
        expect(plPl.formatToParts(1234)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "\u00a0" },
            { type: "integer", value: "234" },
        ]);
        expect(plPl.formatToParts(12345)).toEqual([
            { type: "integer", value: "12" },
            { type: "group", value: "\u00a0" },
            { type: "integer", value: "345" },
        ]);
    });

    test("useGrouping=auto", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "auto" });
        expect(en.formatToParts(123456)).toEqual([
            { type: "integer", value: "123" },
            { type: "group", value: "," },
            { type: "integer", value: "456" },
        ]);
        expect(en.formatToParts(1234567)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "," },
            { type: "integer", value: "234" },
            { type: "group", value: "," },
            { type: "integer", value: "567" },
        ]);

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: "auto" });
        expect(enIn.formatToParts(123456)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "," },
            { type: "integer", value: "23" },
            { type: "group", value: "," },
            { type: "integer", value: "456" },
        ]);
        expect(enIn.formatToParts(1234567)).toEqual([
            { type: "integer", value: "12" },
            { type: "group", value: "," },
            { type: "integer", value: "34" },
            { type: "group", value: "," },
            { type: "integer", value: "567" },
        ]);

        const ar = new Intl.NumberFormat("ar", { useGrouping: "auto" });
        expect(ar.formatToParts(123456)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0664\u0665\u0666" },
        ]);
        expect(ar.formatToParts(1234567)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0662\u0663\u0664" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0665\u0666\u0667" },
        ]);
    });

    test("useGrouping=min2", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: "min2" });
        expect(en.formatToParts(1234)).toEqual([{ type: "integer", value: "1234" }]);
        expect(en.formatToParts(12345)).toEqual([
            { type: "integer", value: "12" },
            { type: "group", value: "," },
            { type: "integer", value: "345" },
        ]);

        const plPl = new Intl.NumberFormat("pl-PL", { useGrouping: "min2" });
        expect(plPl.formatToParts(1234)).toEqual([{ type: "integer", value: "1234" }]);
        expect(plPl.formatToParts(12345)).toEqual([
            { type: "integer", value: "12" },
            { type: "group", value: "\u00a0" },
            { type: "integer", value: "345" },
        ]);
    });

    test("useGrouping=false", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: false });
        expect(en.formatToParts(123456)).toEqual([{ type: "integer", value: "123456" }]);
        expect(en.formatToParts(1234567)).toEqual([{ type: "integer", value: "1234567" }]);

        const enIn = new Intl.NumberFormat("en-IN", { useGrouping: false });
        expect(enIn.formatToParts(123456)).toEqual([{ type: "integer", value: "123456" }]);
        expect(enIn.formatToParts(1234567)).toEqual([{ type: "integer", value: "1234567" }]);

        const ar = new Intl.NumberFormat("ar", { useGrouping: false });
        expect(ar.formatToParts(123456)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663\u0664\u0665\u0666" },
        ]);
        expect(ar.formatToParts(1234567)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663\u0664\u0665\u0666\u0667" },
        ]);
    });

    test("notation=scientific", () => {
        const en = new Intl.NumberFormat("en", { notation: "scientific" });
        expect(en.formatToParts(12.3)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
            { type: "exponentSeparator", value: "E" },
            { type: "exponentInteger", value: "1" },
        ]);
        expect(en.formatToParts(0.12)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "exponentSeparator", value: "E" },
            { type: "exponentMinusSign", value: "-" },
            { type: "exponentInteger", value: "1" },
        ]);

        const ar = new Intl.NumberFormat("ar", { notation: "scientific" });
        expect(ar.formatToParts(12.3)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
            { type: "exponentSeparator", value: "\u0623\u0633" },
            { type: "exponentInteger", value: "\u0661" },
        ]);
        expect(ar.formatToParts(0.12)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "exponentSeparator", value: "\u0623\u0633" },
            { type: "literal", value: "\u061c" },
            { type: "exponentMinusSign", value: "-" },
            { type: "exponentInteger", value: "\u0661" },
        ]);
    });

    test("notation=engineering", () => {
        const en = new Intl.NumberFormat("en", { notation: "engineering" });
        expect(en.formatToParts(1234)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "234" },
            { type: "exponentSeparator", value: "E" },
            { type: "exponentInteger", value: "3" },
        ]);
        expect(en.formatToParts(0.12)).toEqual([
            { type: "integer", value: "120" },
            { type: "exponentSeparator", value: "E" },
            { type: "exponentMinusSign", value: "-" },
            { type: "exponentInteger", value: "3" },
        ]);

        const ar = new Intl.NumberFormat("ar", { notation: "engineering" });
        expect(ar.formatToParts(1234)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663\u0664" },
            { type: "exponentSeparator", value: "\u0623\u0633" },
            { type: "exponentInteger", value: "\u0663" },
        ]);
        expect(ar.formatToParts(0.12)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0660" },
            { type: "exponentSeparator", value: "\u0623\u0633" },
            { type: "literal", value: "\u061c" },
            { type: "exponentMinusSign", value: "-" },
            { type: "exponentInteger", value: "\u0663" },
        ]);
    });

    test("notation=compact, compactDisplay=long", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact", compactDisplay: "long" });
        expect(en.formatToParts(1200)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "compact", value: "thousand" },
        ]);
        expect(en.formatToParts(12900000)).toEqual([
            { type: "integer", value: "13" },
            { type: "literal", value: " " },
            { type: "compact", value: "million" },
        ]);

        const ar = new Intl.NumberFormat("ar", { notation: "compact", compactDisplay: "long" });
        expect(ar.formatToParts(1200)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "literal", value: " " },
            { type: "compact", value: "ألف" },
        ]);
        expect(ar.formatToParts(12900000)).toEqual([
            { type: "integer", value: "\u0661\u0663" },
            { type: "literal", value: " " },
            { type: "compact", value: "مليون" },
        ]);
    });

    test("notation=compact, compactDisplay=short", () => {
        const en = new Intl.NumberFormat("en", { notation: "compact", compactDisplay: "short" });
        expect(en.formatToParts(1200)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "compact", value: "K" },
        ]);
        expect(en.formatToParts(12900000)).toEqual([
            { type: "integer", value: "13" },
            { type: "compact", value: "M" },
        ]);

        const ar = new Intl.NumberFormat("ar", { notation: "compact", compactDisplay: "short" });
        expect(ar.formatToParts(1200)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "literal", value: "\u00a0" },
            { type: "compact", value: "ألف" },
        ]);
        expect(ar.formatToParts(12900000)).toEqual([
            { type: "integer", value: "\u0661\u0663" },
            { type: "literal", value: "\u00a0" },
            { type: "compact", value: "مليون" },
        ]);
    });
});

describe("style=percent", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", minimumFractionDigits: 2 });
        expect(en.formatToParts(1)).toEqual([
            { type: "integer", value: "100" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(1.2345)).toEqual([
            { type: "integer", value: "123" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "45" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", minimumFractionDigits: 2 });
        expect(ar.formatToParts(1)).toEqual([
            { type: "integer", value: "\u0661\u0660\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(1.2345)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0664\u0665" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });

    test("signDisplay=never", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "never" });
        expect(en.formatToParts(0.01)).toEqual([
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.01)).toEqual([
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "never" });
        expect(ar.formatToParts(0.01)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.01)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });

    test("signDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "auto" });
        expect(en.formatToParts(0.0)).toEqual([
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(0.01)).toEqual([
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.01)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "auto" });
        expect(ar.formatToParts(0.0)).toEqual([
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(0.01)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });

    test("signDisplay=always", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "always" });
        expect(en.formatToParts(0.0)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(0.01)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.01)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "always" });
        expect(ar.formatToParts(0.0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });

    test("signDisplay=exceptZero", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "exceptZero" });
        expect(en.formatToParts(0.0)).toEqual([
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(0.01)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.0)).toEqual([
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.01)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "exceptZero" });
        expect(ar.formatToParts(0.0)).toEqual([
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.0)).toEqual([
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });

    test("signDisplay=negative", () => {
        const en = new Intl.NumberFormat("en", { style: "percent", signDisplay: "negative" });
        expect(en.formatToParts(0.0)).toEqual([
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(0.01)).toEqual([
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.0)).toEqual([
            { type: "integer", value: "0" },
            { type: "percentSign", value: "%" },
        ]);
        expect(en.formatToParts(-0.01)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "integer", value: "1" },
            { type: "percentSign", value: "%" },
        ]);

        const ar = new Intl.NumberFormat("ar", { style: "percent", signDisplay: "negative" });
        expect(ar.formatToParts(0.0)).toEqual([
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(0.01)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.0)).toEqual([
            { type: "integer", value: "\u0660" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
        expect(ar.formatToParts(-0.01)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "integer", value: "\u0661" },
            { type: "percentSign", value: "\u066a" },
            { type: "literal", value: "\u061c" },
        ]);
    });
});

describe("style=currency", () => {
    test("currencyDisplay=code", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "code",
        });
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "USD" },
            { type: "literal", value: "\u00a0" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1.23)).toEqual([
            { type: "currency", value: "USD" },
            { type: "literal", value: "\u00a0" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "code",
        });
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "USD" },
        ]);
        expect(ar.formatToParts(1.23)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "USD" },
        ]);
    });

    test("currencyDisplay=symbol", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "symbol",
        });
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1.23)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "symbol",
        });
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1.23)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
    });

    test("currencyDisplay=narrowSymbol", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "narrowSymbol",
        });
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1.23)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "narrowSymbol",
        });
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1.23)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
    });

    test("currencyDisplay=name", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "name",
        });
        expect(en.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
            { type: "literal", value: " " },
            { type: "currency", value: "US dollars" },
        ]);
        expect(en.formatToParts(1.23)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "23" },
            { type: "literal", value: " " },
            { type: "currency", value: "US dollars" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            currencyDisplay: "name",
        });
        expect(ar.formatToParts(1)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: " " },
            { type: "currency", value: "دولار أمريكي" },
        ]);
        expect(ar.formatToParts(1.23)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662\u0663" },
            { type: "literal", value: " " },
            { type: "currency", value: "دولار أمريكي" },
        ]);
    });

    test("signDisplay=never", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "never",
        });
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "never",
        });
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);

        const zh = new Intl.NumberFormat("zh-Hant-u-nu-hanidec", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "never",
        });
        expect(zh.formatToParts(1)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-1)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
    });

    test("signDisplay=auto", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "auto",
        });
        expect(en.formatToParts(0)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "auto",
        });
        expect(ar.formatToParts(0)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);

        const zh = new Intl.NumberFormat("zh-Hant-u-nu-hanidec", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "auto",
        });
        expect(zh.formatToParts(0)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(1)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-0)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
        expect(zh.formatToParts(-1)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
    });

    test("signDisplay=always", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "always",
        });
        expect(en.formatToParts(0)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "always",
        });
        expect(ar.formatToParts(0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);

        const zh = new Intl.NumberFormat("zh-Hant-u-nu-hanidec", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "always",
        });
        expect(zh.formatToParts(0)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-0)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
        expect(zh.formatToParts(-1)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
    });

    test("signDisplay=exceptZero", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "exceptZero",
        });
        expect(en.formatToParts(0)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "exceptZero",
        });
        expect(ar.formatToParts(0)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "plusSign", value: "+" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);

        const zh = new Intl.NumberFormat("zh-Hant-u-nu-hanidec", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "exceptZero",
        });
        expect(zh.formatToParts(0)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(1)).toEqual([
            { type: "plusSign", value: "+" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-0)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-1)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
    });

    test("signDisplay=negative", () => {
        const en = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            signDisplay: "negative",
        });
        expect(en.formatToParts(0)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(1)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-0)).toEqual([
            { type: "currency", value: "$" },
            { type: "integer", value: "0" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);
        expect(en.formatToParts(-1)).toEqual([
            { type: "minusSign", value: "-" },
            { type: "currency", value: "$" },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "00" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "currency",
            currency: "USD",
            signDisplay: "negative",
        });
        expect(ar.formatToParts(0)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(1)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-0)).toEqual([
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0660" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);
        expect(ar.formatToParts(-1)).toEqual([
            { type: "literal", value: "\u061c" },
            { type: "minusSign", value: "-" },
            { type: "literal", value: "\u200f" },
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0660\u0660" },
            { type: "literal", value: "\u00a0" },
            { type: "currency", value: "US$" },
        ]);

        const zh = new Intl.NumberFormat("zh-Hant-u-nu-hanidec", {
            style: "currency",
            currency: "USD",
            currencySign: "accounting",
            signDisplay: "negative",
        });
        expect(zh.formatToParts(0)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(1)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-0)).toEqual([
            { type: "currency", value: "US$" },
            { type: "integer", value: "〇" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
        ]);
        expect(zh.formatToParts(-1)).toEqual([
            { type: "literal", value: "(" },
            { type: "currency", value: "US$" },
            { type: "integer", value: "一" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "〇〇" },
            { type: "literal", value: ")" },
        ]);
    });
});

describe("style=unit", () => {
    test("unitDisplay=long", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "long",
        });
        expect(en1.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "gigabit" },
        ]);
        expect(en1.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "gigabits" },
        ]);

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "long",
        });
        expect(en2.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "kilometer per hour" },
        ]);
        expect(en2.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "kilometers per hour" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "long",
        });
        expect(ar.formatToParts(1)).toEqual([{ type: "unit", value: "قدم" }]);
        expect(ar.formatToParts(1.2)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "literal", value: " " },
            { type: "unit", value: "قدم" },
        ]);

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "long",
        });
        expect(ja.formatToParts(1)).toEqual([
            { type: "unit", value: "時速" },
            { type: "literal", value: " " },
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "キロメートル" },
        ]);
        expect(ja.formatToParts(1.2)).toEqual([
            { type: "unit", value: "時速" },
            { type: "literal", value: " " },
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "キロメートル" },
        ]);
    });

    test("unitDisplay=short", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "short",
        });
        expect(en1.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "Gb" },
        ]);
        expect(en1.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "Gb" },
        ]);

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "short",
        });
        expect(en2.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "km/h" },
        ]);
        expect(en2.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "km/h" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "short",
        });
        expect(ar.formatToParts(1)).toEqual([{ type: "unit", value: "قدم" }]);
        expect(ar.formatToParts(1.2)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "literal", value: " " },
            { type: "unit", value: "قدم" },
        ]);

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "short",
        });
        expect(ja.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "literal", value: " " },
            { type: "unit", value: "km/h" },
        ]);
        expect(ja.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "literal", value: " " },
            { type: "unit", value: "km/h" },
        ]);
    });

    test("unitDisplay=narrow", () => {
        const en1 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "gigabit",
            unitDisplay: "narrow",
        });
        expect(en1.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "unit", value: "Gb" },
        ]);
        expect(en1.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "unit", value: "Gb" },
        ]);

        const en2 = new Intl.NumberFormat("en", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "narrow",
        });
        expect(en2.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "unit", value: "km/h" },
        ]);
        expect(en2.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "unit", value: "km/h" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            style: "unit",
            unit: "foot",
            unitDisplay: "narrow",
        });
        expect(ar.formatToParts(1)).toEqual([{ type: "unit", value: "قدم" }]);
        expect(ar.formatToParts(1.2)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "decimal", value: "\u066b" },
            { type: "fraction", value: "\u0662" },
            { type: "literal", value: " " },
            { type: "unit", value: "قدم" },
        ]);

        const ja = new Intl.NumberFormat("ja", {
            style: "unit",
            unit: "kilometer-per-hour",
            unitDisplay: "narrow",
        });
        expect(ja.formatToParts(1)).toEqual([
            { type: "integer", value: "1" },
            { type: "unit", value: "km/h" },
        ]);
        expect(ja.formatToParts(1.2)).toEqual([
            { type: "integer", value: "1" },
            { type: "decimal", value: "." },
            { type: "fraction", value: "2" },
            { type: "unit", value: "km/h" },
        ]);
    });
});

describe("bigint", () => {
    test("default", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatToParts(123456n)).toEqual([
            { type: "integer", value: "123" },
            { type: "group", value: "," },
            { type: "integer", value: "456" },
        ]);

        const ar = new Intl.NumberFormat("ar");
        expect(ar.formatToParts(123456n)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0664\u0665\u0666" },
        ]);
    });

    test("useGrouping=false", () => {
        const en = new Intl.NumberFormat("en", { useGrouping: false });
        expect(en.formatToParts(123456n)).toEqual([{ type: "integer", value: "123456" }]);

        const ar = new Intl.NumberFormat("ar", { useGrouping: false });
        expect(ar.formatToParts(123456n)).toEqual([
            { type: "integer", value: "\u0661\u0662\u0663\u0664\u0665\u0666" },
        ]);
    });

    test("significant digits", () => {
        const en = new Intl.NumberFormat("en", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(en.formatToParts(1234567n)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "," },
            { type: "integer", value: "234" },
            { type: "group", value: "," },
            { type: "integer", value: "570" },
        ]);
        expect(en.formatToParts(1234561n)).toEqual([
            { type: "integer", value: "1" },
            { type: "group", value: "," },
            { type: "integer", value: "234" },
            { type: "group", value: "," },
            { type: "integer", value: "560" },
        ]);

        const ar = new Intl.NumberFormat("ar", {
            minimumSignificantDigits: 4,
            maximumSignificantDigits: 6,
        });
        expect(ar.formatToParts(1234567n)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0662\u0663\u0664" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0665\u0667\u0660" },
        ]);
        expect(ar.formatToParts(1234561n)).toEqual([
            { type: "integer", value: "\u0661" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0662\u0663\u0664" },
            { type: "group", value: "\u066c" },
            { type: "integer", value: "\u0665\u0666\u0660" },
        ]);
    });
});
