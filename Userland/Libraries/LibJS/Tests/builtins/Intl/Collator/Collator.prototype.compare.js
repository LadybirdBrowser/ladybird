describe("correct behavior", () => {
    test("length is 2", () => {
        expect(new Intl.Collator().compare).toHaveLength(2);
    });

    test("name is empty string", () => {
        expect(new Intl.Collator().compare.name).toBe("");
    });

    test("basic functionality", () => {
        const collator = new Intl.Collator();
        expect(collator.compare("", "")).toBe(0);
        expect(collator.compare("a", "a")).toBe(0);
        expect(collator.compare("6", "6")).toBe(0);

        function compareBoth(a, b) {
            const aTob = collator.compare(a, b);
            const bToa = collator.compare(b, a);

            expect(aTob).toBe(1);
            expect(bToa).toBe(-1);
        }

        compareBoth("a", "");
        compareBoth("1", "");
        compareBoth("A", "a");
        compareBoth("7", "3");
        compareBoth("0000", "0");

        expect(collator.compare("undefined")).toBe(0);
        expect(collator.compare("undefined", undefined)).toBe(0);

        expect(collator.compare("null", null)).toBe(0);
        expect(collator.compare("null", undefined)).toBe(-1);
        expect(collator.compare("null")).toBe(-1);
    });

    test("canonically equivalent strings", () => {
        var tests = [
            ["ä\u0306", "a\u0308\u0306"],
            ["ă\u0308", "a\u0306\u0308"],
            ["ạ\u0308", "a\u0323\u0308"],
            ["a\u0308\u0323", "a\u0323\u0308"],
            ["ä\u0323", "a\u0323\u0308"],
            ["Å", "Å"],
            ["Å", "A\u030A"],
            ["Ç", "C\u0327"],
            ["ḋ\u0323", "ḍ\u0307"],
            ["ḋ\u0323", "d\u0323\u0307"],
            ["ô", "o\u0302"],
            ["ö", "o\u0308"],
            ["q\u0307\u0323", "q\u0323\u0307"],
            ["ṩ", "s\u0323\u0307"],
            ["ự", "ụ\u031B"],
            ["ự", "u\u031B\u0323"],
            ["ự", "ư\u0323"],
            ["ự", "u\u0323\u031B"],
            ["Ω", "Ω"],
            ["x\u031B\u0323", "x\u0323\u031B"],
            ["퓛", "\u1111\u1171\u11B6"],
            ["北", "\uD87E\uDC2B"],
            ["가", "\u1100\u1161"],
            ["\uD834\uDD5E", "\uD834\uDD57\uD834\uDD65"],
        ];

        const en = new Intl.Collator("en");
        const ja = new Intl.Collator("ja");
        const th = new Intl.Collator("th");

        tests.forEach(test => {
            expect(en.compare(test[0], test[1])).toBe(0);
            expect(ja.compare(test[0], test[1])).toBe(0);
            expect(th.compare(test[0], test[1])).toBe(0);
        });
    });

    test("ignorePunctuation", () => {
        [undefined, true, false].forEach(ignorePunctuation => {
            let expected = false;

            const en = new Intl.Collator("en", { ignorePunctuation });
            expect(en.compare("", " ")).toBe(en.resolvedOptions().ignorePunctuation ? 0 : -1);
            expect(en.compare("", ",")).toBe(en.resolvedOptions().ignorePunctuation ? 0 : -1);

            const ja = new Intl.Collator("ja", { ignorePunctuation });
            expect(ja.compare("", " ")).toBe(ja.resolvedOptions().ignorePunctuation ? 0 : -1);
            expect(ja.compare("", ",")).toBe(ja.resolvedOptions().ignorePunctuation ? 0 : -1);

            const th = new Intl.Collator("th", { ignorePunctuation });
            expect(th.compare("", " ")).toBe(th.resolvedOptions().ignorePunctuation ? 0 : -1);
            expect(th.compare("", ",")).toBe(th.resolvedOptions().ignorePunctuation ? 0 : -1);
        });
    });

    test("UTF-16", () => {
        const collator = new Intl.Collator();
        const string = "😀😀";
        expect(collator.compare(string, "😀😀")).toBe(0);
        expect(collator.compare(string, "\ud83d") > 0);
        expect(collator.compare(string, "😀😀s") < 0);
    });
});
