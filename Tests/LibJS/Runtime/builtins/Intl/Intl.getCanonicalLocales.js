describe("errors", () => {
    test("invalid tag", () => {
        expect(() => {
            Intl.getCanonicalLocales("EN_US");
        }).toThrowWithMessage(RangeError, "EN_US is not a structurally valid language tag");
    });

    test("root tag", () => {
        expect(() => {
            Intl.getCanonicalLocales("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");
    });

    test("no language tag", () => {
        expect(() => {
            Intl.getCanonicalLocales("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");
    });

    test("duplicate variant subtags", () => {
        expect(() => {
            Intl.getCanonicalLocales("en-posix-POSIX");
        }).toThrowWithMessage(RangeError, "en-posix-POSIX is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("en-POSIX-POSIX");
        }).toThrowWithMessage(RangeError, "en-POSIX-POSIX is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("en-1aaa-1BBB-1AAA");
        }).toThrowWithMessage(RangeError, "en-1aaa-1BBB-1AAA is not a structurally valid language tag");
    });

    test("well-formed tags that ICU cannot represent", () => {
        const subtags = (count, prefix, width) =>
            Array.from({ length: count }, (_, i) => prefix + i.toString(36).padStart(width, "0")).join("-");

        const tags = [
            "ja-Latn-hepburn-heploc-alalc97",
            "aa-" + subtags(26, "a", 5),
            "aa-u-" + [..."abcdefghijklmnopqrstuvwxyz"].map(key => `0${key}`).join("-"),
            "aa-" + [..."0123456789abcdefghijklmnop"].map(key => `${key}-aa`).join("-"),
            "aa-t-aa-" + subtags(18, "a", 7) + "-b0000-c0000-d00000",
            "aa-x-lvariant-" + subtags(60, "", 2) + "a",
            "aa-u-" + [..."0123456789abcdefghijklmn"].map(key => `${key}a`).join("-") + "-y-aa-z-aa",
            "aa-" + subtags(12, "a", 5) + "-x-lvariant-" + subtags(32, "b", 1) + "a",
        ];

        for (const tag of tags) {
            expect(() => {
                Intl.getCanonicalLocales(tag);
            }).toThrowWithMessage(RangeError, `${tag} is not a supported language tag`);
        }
    });

    test("improperly placed separator", () => {
        expect(() => {
            Intl.getCanonicalLocales("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("-en");
        }).toThrowWithMessage(RangeError, "-en is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("en--US");
        }).toThrowWithMessage(RangeError, "en--US is not a structurally valid language tag");
    });

    test("non string or object locale", () => {
        expect(() => {
            Intl.getCanonicalLocales([true]);
        }).toThrowWithMessage(TypeError, "true is neither an object nor a string");
    });

    test("duplicate extension components", () => {
        expect(() => {
            Intl.getCanonicalLocales("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("en-t-aa-T-aa");
        }).toThrowWithMessage(RangeError, "en-t-aa-T-aa is not a structurally valid language tag");

        expect(() => {
            Intl.getCanonicalLocales("en-z-aa-Z-aa");
        }).toThrowWithMessage(RangeError, "en-z-aa-Z-aa is not a structurally valid language tag");
    });

    test("duplicate transformed extension variant subtags", () => {
        expect(() => {
            Intl.getCanonicalLocales("en-t-en-POSIX-POSIX");
        }).toThrowWithMessage(RangeError, "en-t-en-POSIX-POSIX is not a structurally valid language tag");
    });

    test("non-ASCII UTF-16 extension subtags", () => {
        ["en-\uff55-ab-foo", "en-u-\uff41b-foo", "en-t-\uff4b0-foo"].forEach(locale => {
            expect(() => {
                Intl.getCanonicalLocales(locale);
            }).toThrowWithMessage(RangeError, `${locale} is not a structurally valid language tag`);
        });
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Intl.getCanonicalLocales).toHaveLength(1);
    });

    test("valid locales", () => {
        expect(Intl.getCanonicalLocales([])).toEqual([]);
        expect(Intl.getCanonicalLocales("EN-US")).toEqual(["en-US"]);
        expect(Intl.getCanonicalLocales(["EN-US"])).toEqual(["en-US"]);
        expect(Intl.getCanonicalLocales(["EN-US", "Fr"])).toEqual(["en-US", "fr"]);
        expect(Intl.getCanonicalLocales("EN-lATN-US")).toEqual(["en-Latn-US"]);
        expect(Intl.getCanonicalLocales("EN-US-POSIX")).toEqual(["en-US-u-va-posix"]);
        expect(Intl.getCanonicalLocales("EN-LATN-US-POSIX")).toEqual(["en-Latn-US-u-va-posix"]);
    });

    test("duplicate locales", () => {
        expect(Intl.getCanonicalLocales(["EN-US", "en-US", "en-us"])).toEqual(["en-US"]);
    });

    test("non-array object", () => {
        expect(Intl.getCanonicalLocales({})).toEqual([]);
        expect(Intl.getCanonicalLocales({ en: 123 })).toEqual([]);
        expect(Intl.getCanonicalLocales(undefined)).toEqual([]);
        expect(Intl.getCanonicalLocales(true)).toEqual([]);
        expect(Intl.getCanonicalLocales(123)).toEqual([]);
    });

    test("duplicate Unicode locale extension attributes", () => {
        expect(Intl.getCanonicalLocales("en-us-u-aaa-aaa")).toEqual(["en-US-u-aaa"]);
        expect(Intl.getCanonicalLocales("en-us-u-aaa-bbb-aaa")).toEqual(["en-US-u-aaa-bbb"]);
    });

    test("duplicate Unicode locale extension keywords", () => {
        expect(Intl.getCanonicalLocales("en-us-u-1k-aaa-1k-bbb")).toEqual(["en-US-u-1k-aaa"]);
        expect(Intl.getCanonicalLocales("en-us-u-1k-aaa-2k-ccc-1k-bbb")).toEqual(["en-US-u-1k-aaa-2k-ccc"]);
    });

    test("canonicalize locale objects", () => {
        const en = new Intl.Locale("en", { script: "Latn" });
        expect(Intl.getCanonicalLocales(en)).toEqual(["en-Latn"]);

        const es = new Intl.Locale("es", { region: "419" });
        expect(Intl.getCanonicalLocales([en, es])).toEqual(["en-Latn", "es-419"]);
    });
});
