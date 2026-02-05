describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.Locale();
        }).toThrowWithMessage(TypeError, "Intl.Locale constructor must be called with 'new'");
    });

    test("tag is neither object nor string", () => {
        expect(() => {
            new Intl.Locale(1);
        }).toThrowWithMessage(TypeError, "tag is neither an object nor a string");
    });

    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.Locale("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.Locale("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.Locale("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.Locale("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("locale option fields are not valid Unicode TR35 subtags", () => {
        expect(() => {
            new Intl.Locale("en", { language: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option language");

        expect(() => {
            new Intl.Locale("en", { script: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option script");

        expect(() => {
            new Intl.Locale("en", { region: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option region");
    });

    test("keyword option fields are not valid Unicode TR35 types", () => {
        expect(() => {
            new Intl.Locale("en", { calendar: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option calendar");

        expect(() => {
            new Intl.Locale("en", { collation: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option collation");

        expect(() => {
            new Intl.Locale("en", { hourCycle: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option hourCycle");

        expect(() => {
            new Intl.Locale("en", { caseFirst: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option caseFirst");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Intl.Locale).toHaveLength(1);
    });

    test("locale option fields create subtags", () => {
        const sr = new Intl.Locale("sr", { script: "Latn" });
        expect(sr.toString()).toBe("sr-Latn");

        const en = new Intl.Locale("en", { region: "US" });
        expect(en.toString()).toBe("en-US");
    });

    test("locale option fields replace subtags", () => {
        const es = new Intl.Locale("en", { language: "es" });
        expect(es.toString()).toBe("es");

        const sr = new Intl.Locale("sr-Latn", { script: "Cyrl" });
        expect(sr.toString()).toBe("sr-Cyrl");

        const en = new Intl.Locale("en-US", { region: "GB" });
        expect(en.toString()).toBe("en-GB");
    });

    test("construction from another locale", () => {
        const en1 = new Intl.Locale("en", { region: "US" });
        const en2 = new Intl.Locale(en1, { script: "Latn" });
        expect(en2.toString()).toBe("en-Latn-US");
    });

    test("locale is canonicalized", () => {
        const en = new Intl.Locale("EN", { script: "lAtN", region: "us" });
        expect(en.toString()).toBe("en-Latn-US");
    });

    test("calendar extension", () => {
        const en1 = new Intl.Locale("en-u-ca-abc");
        expect(en1.toString()).toBe("en-u-ca-abc");

        const en2 = new Intl.Locale("en", { calendar: "abc" });
        expect(en2.toString()).toBe("en-u-ca-abc");

        const en3 = new Intl.Locale("en-u-ca-abc", { calendar: "def" });
        expect(en3.toString()).toBe("en-u-ca-def");
    });

    test("collation extension", () => {
        const en1 = new Intl.Locale("en-u-co-abc");
        expect(en1.toString()).toBe("en-u-co-abc");

        const en2 = new Intl.Locale("en", { collation: "abc" });
        expect(en2.toString()).toBe("en-u-co-abc");

        const en3 = new Intl.Locale("en-u-co-abc", { collation: "def" });
        expect(en3.toString()).toBe("en-u-co-def");
    });

    test("hour-cycle extension", () => {
        const en1 = new Intl.Locale("en-u-hc-h11");
        expect(en1.toString()).toBe("en-u-hc-h11");

        const en2 = new Intl.Locale("en", { hourCycle: "h12" });
        expect(en2.toString()).toBe("en-u-hc-h12");

        const en3 = new Intl.Locale("en-u-hc-h23", { hourCycle: "h24" });
        expect(en3.toString()).toBe("en-u-hc-h24");
    });

    test("case-first extension", () => {
        const en1 = new Intl.Locale("en-u-kf-upper");
        expect(en1.toString()).toBe("en-u-kf-upper");

        const en2 = new Intl.Locale("en", { caseFirst: "lower" });
        expect(en2.toString()).toBe("en-u-kf-lower");

        const en3 = new Intl.Locale("en-u-kf-upper", { caseFirst: "false" });
        expect(en3.toString()).toBe("en-u-kf-false");
    });

    test("numeric extension", () => {
        // Note: "true" values are removed from Unicode locale extensions during canonicalization.
        const en1 = new Intl.Locale("en-u-kn-true");
        expect(en1.toString()).toBe("en-u-kn");

        const en2 = new Intl.Locale("en", { numeric: false });
        expect(en2.toString()).toBe("en-u-kn-false");

        const en3 = new Intl.Locale("en-u-kn-false", { numeric: 1 });
        expect(en3.toString()).toBe("en-u-kn");
    });

    test("numbering-system extension", () => {
        const en1 = new Intl.Locale("en-u-nu-abc");
        expect(en1.toString()).toBe("en-u-nu-abc");

        const en2 = new Intl.Locale("en", { numberingSystem: "abc" });
        expect(en2.toString()).toBe("en-u-nu-abc");

        const en3 = new Intl.Locale("en-u-nu-abc", { numberingSystem: "def" });
        expect(en3.toString()).toBe("en-u-nu-def");
    });

    test("unicode extension inserted before private use extension", () => {
        const en1 = new Intl.Locale("en-x-abcd", { calendar: "abc" });
        expect(en1.toString()).toBe("en-u-ca-abc-x-abcd");
    });
});
