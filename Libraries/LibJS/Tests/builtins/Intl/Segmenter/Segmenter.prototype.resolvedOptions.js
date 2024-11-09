describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.Segmenter.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = new Intl.Segmenter("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en2 = new Intl.Segmenter("en-u-nu-latn");
        expect(en2.resolvedOptions().locale).toBe("en");

        const en3 = new Intl.Segmenter("en-u-ca-islamicc-nu-latn");
        expect(en3.resolvedOptions().locale).toBe("en");
    });

    test("granularity", () => {
        const en1 = new Intl.Segmenter("en");
        expect(en1.resolvedOptions().granularity).toBe("grapheme");

        ["grapheme", "word", "sentence"].forEach(granularity => {
            const en2 = new Intl.Segmenter("en", { granularity: granularity });
            expect(en2.resolvedOptions().granularity).toBe(granularity);
        });
    });
});
