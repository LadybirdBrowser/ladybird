describe("errors", () => {
    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.Collator("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.Collator("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.Collator("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.Collator("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.Collator("en", null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("localeMatcher option is invalid ", () => {
        expect(() => {
            new Intl.Collator("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("usage option is invalid ", () => {
        expect(() => {
            new Intl.Collator("en", { usage: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option usage");
    });

    test("collation option is invalid ", () => {
        expect(() => {
            new Intl.Collator("en", { collation: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option collation");
    });

    test("caseFirst option is invalid ", () => {
        expect(() => {
            new Intl.Collator("en", { caseFirst: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option caseFirst");
    });

    test("sensitivity option is invalid ", () => {
        expect(() => {
            new Intl.Collator("en", { sensitivity: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option sensitivity");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.Collator).toHaveLength(0);
    });

    test("all valid usage options", () => {
        ["sort", "search"].forEach(usage => {
            expect(() => {
                new Intl.Collator("en", { usage: usage });
            }).not.toThrow();
        });
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.Collator("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("valid collation options", () => {
        ["default", "compat"].forEach(collation => {
            expect(() => {
                new Intl.Collator("en", { collation: collation });
            }).not.toThrow();
        });
    });

    test("valid caseFirst options", () => {
        ["upper", "lower", "false"].forEach(caseFirst => {
            expect(() => {
                new Intl.Collator("en", { caseFirst: caseFirst });
            }).not.toThrow();
        });
    });

    test("valid sensitivity options", () => {
        ["base", "accent", "case", "variant"].forEach(sensitivity => {
            expect(() => {
                new Intl.Collator("en", { sensitivity: sensitivity });
            }).not.toThrow();
        });
    });
});
