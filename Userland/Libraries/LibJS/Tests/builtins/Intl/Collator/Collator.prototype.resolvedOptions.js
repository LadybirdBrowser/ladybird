describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.Collator.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = new Intl.Collator("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en2 = new Intl.Collator("en-u-kf-upper");
        expect(en2.resolvedOptions().locale).toBe("en-u-kf-upper");

        const en3 = new Intl.Collator("en-u-ca-islamicc-kf-upper");
        expect(en3.resolvedOptions().locale).toBe("en-u-kf-upper");
    });

    test("usage", () => {
        const en1 = new Intl.Collator("en");
        expect(en1.resolvedOptions().usage).toBe("sort");

        ["sort", "search"].forEach(usage => {
            const en2 = new Intl.Collator("en", { usage: usage });
            expect(en2.resolvedOptions().usage).toBe(usage);
        });
    });

    test("sensitivity", () => {
        const en1 = new Intl.Collator("en");
        expect(en1.resolvedOptions().sensitivity).toBe("variant");

        const en2 = new Intl.Collator("en", { usage: "search" });
        expect(en2.resolvedOptions().sensitivity).toBe("variant");

        ["base", "accent", "case", "variant"].forEach(sensitivity => {
            const en3 = new Intl.Collator("en", { sensitivity: sensitivity });
            expect(en3.resolvedOptions().sensitivity).toBe(sensitivity);
        });
    });

    test("ignorePunctuation", () => {
        const en1 = new Intl.Collator("en");
        expect(en1.resolvedOptions().ignorePunctuation).toBeFalse();

        const th1 = new Intl.Collator("th");
        expect(th1.resolvedOptions().ignorePunctuation).toBeTrue();

        [true, false].forEach(ignorePunctuation => {
            const en2 = new Intl.Collator("en", { ignorePunctuation: ignorePunctuation });
            expect(en2.resolvedOptions().ignorePunctuation).toBe(ignorePunctuation);

            const th2 = new Intl.Collator("th", { ignorePunctuation: ignorePunctuation });
            expect(th2.resolvedOptions().ignorePunctuation).toBe(ignorePunctuation);
        });
    });

    test("collation", () => {
        // Only "default" collation is parsed for now.
        const en = new Intl.Collator("en");
        expect(en.resolvedOptions().collation).toBe("default");

        const el = new Intl.Collator("el", { collation: "foo" });
        expect(el.resolvedOptions().collation).toBe("default");
    });

    test("numeric may be set by locale extension", () => {
        const en = new Intl.Collator("en-u-kn");
        expect(en.resolvedOptions().numeric).toBeTrue();

        const el = new Intl.Collator("el-u-kn-false");
        expect(el.resolvedOptions().numeric).toBeFalse();
    });

    test("numeric option overrides locale extension", () => {
        const el = new Intl.Collator("el-u-kn", { numeric: false });
        expect(el.resolvedOptions().numeric).toBeFalse();
    });

    test("numeric option limited to known 'kn' values", () => {
        ["false", "foo"].forEach(numeric => {
            const en = new Intl.Collator(`en-u-kn-${numeric}`);
            expect(en.resolvedOptions().numeric).toBeFalse();
        });

        ["false", "foo"].forEach(numeric => {
            const el = new Intl.Collator(`el-u-kn-${numeric}`);
            expect(el.resolvedOptions().numeric).toBeFalse();
        });
    });

    test("caseFirst may be set by locale extension", () => {
        const en = Intl.Collator("en-u-kf-upper");
        expect(en.resolvedOptions().caseFirst).toBe("upper");

        const el = Intl.Collator("el-u-kf-lower");
        expect(el.resolvedOptions().caseFirst).toBe("lower");

        const ar = Intl.Collator("ar-u-kf-false");
        expect(ar.resolvedOptions().caseFirst).toBe("false");
    });

    test("caseFirst option overrides locale extension", () => {
        const el = Intl.Collator("el-u-kf-upper", { caseFirst: "lower" });
        expect(el.resolvedOptions().caseFirst).toBe("lower");
    });

    test("caseFirst option limited to known 'kf' values", () => {
        ["false", "foo"].forEach(caseFirst => {
            const en = Intl.Collator(`en-u-kf-${caseFirst}`);
            expect(en.resolvedOptions().caseFirst).toBe("false");
        });

        ["false", "foo"].forEach(caseFirst => {
            const el = Intl.Collator(`el-u-kf-${caseFirst}`);
            expect(el.resolvedOptions().caseFirst).toBe("false");
        });
    });
});
