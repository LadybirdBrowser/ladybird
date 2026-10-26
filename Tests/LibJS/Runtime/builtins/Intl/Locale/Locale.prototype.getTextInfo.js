describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getTextInfo();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        const textInfo = new Intl.Locale("en").getTextInfo();

        expect(textInfo).toBeDefined();
        expect(Object.getPrototypeOf(textInfo)).toBe(Object.prototype);

        expect(textInfo.direction).toBeDefined();
        expect(Object.getPrototypeOf(textInfo.direction)).toBe(String.prototype);

        expect(textInfo.direction).toBe("ltr");
        expect(new Intl.Locale("ar").getTextInfo().direction).toBe("rtl");
    });

    test("infer the script from likely subtags", () => {
        expect(new Intl.Locale("und").getTextInfo().direction).toBe("ltr");
        expect(new Intl.Locale("pa").getTextInfo().direction).toBe("ltr");
        expect(new Intl.Locale("pa-PK").getTextInfo().direction).toBe("rtl");
    });

    test("unknown likely script", () => {
        for (const tag of ["xx", "tlh", "qfz", "abcdefgh"]) {
            const textInfo = new Intl.Locale(tag).getTextInfo();
            expect(Object.hasOwn(textInfo, "direction")).toBeTrue();
            expect(textInfo.direction).toBeUndefined();
        }
    });

    test("explicit script takes precedence over the language", () => {
        for (const language of ["und", "en", "ar", "tlh", "abcdefgh"]) {
            expect(new Intl.Locale(`${language}-Latn`).getTextInfo().direction).toBe("ltr");
            expect(new Intl.Locale(`${language}-Arab`).getTextInfo().direction).toBe("rtl");
        }
    });

    test("scripts with unknown direction", () => {
        for (const language of ["und", "en", "ar", "tlh", "abcdefgh"]) {
            for (const script of ["Aaaa", "Qaaq", "True", "Zyyy", "Zinh", "Zzzz", "Brai", "Zxxx", "Aran"]) {
                expect(new Intl.Locale(`${language}-${script}`).getTextInfo().direction).toBeUndefined();
            }
        }
    });
});
