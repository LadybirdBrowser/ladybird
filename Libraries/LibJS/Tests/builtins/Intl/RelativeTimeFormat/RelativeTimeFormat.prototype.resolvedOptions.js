describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.RelativeTimeFormat.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = new Intl.RelativeTimeFormat("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en2 = new Intl.RelativeTimeFormat("en-u-nu-latn");
        expect(en2.resolvedOptions().locale).toBe("en-u-nu-latn");

        const en3 = new Intl.RelativeTimeFormat("en-u-ca-islamicc-nu-latn");
        expect(en3.resolvedOptions().locale).toBe("en-u-nu-latn");
    });

    test("numberingSystem may be set by option", () => {
        const en = new Intl.RelativeTimeFormat("en", { numberingSystem: "latn" });
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = new Intl.RelativeTimeFormat("el", { numberingSystem: "latn" });
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem may be set by locale extension", () => {
        const en = new Intl.RelativeTimeFormat("en-u-nu-latn");
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = new Intl.RelativeTimeFormat("el-u-nu-latn");
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem option overrides locale extension", () => {
        const el = new Intl.RelativeTimeFormat("el-u-nu-latn", { numberingSystem: "adlm" });
        expect(el.resolvedOptions().numberingSystem).toBe("adlm");
    });

    test("numberingSystem option limited to known 'nu' values", () => {
        ["latn", "foo"].forEach(numberingSystem => {
            const en = new Intl.RelativeTimeFormat("en", { numberingSystem: numberingSystem });
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "foo"].forEach(numberingSystem => {
            const en = new Intl.RelativeTimeFormat(`en-u-nu-${numberingSystem}`);
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = new Intl.RelativeTimeFormat("el", { numberingSystem: numberingSystem });
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = new Intl.RelativeTimeFormat(`el-u-nu-${numberingSystem}`);
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });
    });

    test("style", () => {
        const en1 = new Intl.RelativeTimeFormat("en");
        expect(en1.resolvedOptions().style).toBe("long");

        ["long", "short", "narrow"].forEach(style => {
            const en2 = new Intl.RelativeTimeFormat("en", { style: style });
            expect(en2.resolvedOptions().style).toBe(style);
        });
    });

    test("numeric", () => {
        const en1 = new Intl.RelativeTimeFormat("en");
        expect(en1.resolvedOptions().numeric).toBe("always");

        ["always", "auto"].forEach(numeric => {
            const en2 = new Intl.RelativeTimeFormat("en", { numeric: numeric });
            expect(en2.resolvedOptions().numeric).toBe(numeric);
        });
    });
});
