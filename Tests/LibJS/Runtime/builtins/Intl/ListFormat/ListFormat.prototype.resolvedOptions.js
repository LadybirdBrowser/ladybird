describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.ListFormat.prototype.resolvedOptions).toHaveLength(0);
    });

    test("all valid types", () => {
        ["conjunction", "disjunction", "unit"].forEach(type => {
            const en = new Intl.ListFormat("en", { type: type });
            expect(en.resolvedOptions()).toEqual({
                locale: "en",
                type: type,
                style: "long",
            });
        });
    });

    test("all valid styles", () => {
        ["long", "short", "narrow"].forEach(style => {
            const en = new Intl.ListFormat("en", { style: style });
            expect(en.resolvedOptions()).toEqual({
                locale: "en",
                type: "conjunction",
                style: style,
            });
        });
    });

    test("locales with extensions", () => {
        const en = new Intl.ListFormat("en-t-en");
        expect(en.resolvedOptions().locale).toBe("en");

        const es419 = new Intl.ListFormat("es-419-u-1k-aaa");
        expect(es419.resolvedOptions().locale).toBe("es-419");

        const zhHant = new Intl.ListFormat(["zh-Hant-x-aaa"]);
        expect(zhHant.resolvedOptions().locale).toBe("zh-Hant");
    });
});
