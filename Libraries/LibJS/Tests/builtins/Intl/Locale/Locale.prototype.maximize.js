test("length is 0", () => {
    expect(Intl.Locale.prototype.maximize).toHaveLength(0);
});

test("normal behavior", () => {
    expect(new Intl.Locale("en").maximize().toString()).toBe("en-Latn-US");

    expect(new Intl.Locale("en-Latn").maximize().toString()).toBe("en-Latn-US");
    expect(new Intl.Locale("en-Shaw").maximize().toString()).toBe("en-Shaw-GB");
    expect(new Intl.Locale("en-Arab").maximize().toString()).toBe("en-Arab-US");

    expect(new Intl.Locale("en-US").maximize().toString()).toBe("en-Latn-US");
    expect(new Intl.Locale("en-GB").maximize().toString()).toBe("en-Latn-GB");
    expect(new Intl.Locale("en-FR").maximize().toString()).toBe("en-Latn-FR");

    expect(new Intl.Locale("it-Kana-CA").maximize().toString()).toBe("it-Kana-CA");

    expect(new Intl.Locale("und").maximize().toString()).toBe("en-Latn-US");
    expect(new Intl.Locale("und-Thai").maximize().toString()).toBe("th-Thai-TH");
    expect(new Intl.Locale("und-419").maximize().toString()).toBe("es-Latn-419");
    expect(new Intl.Locale("und-150").maximize().toString()).toBe("en-Latn-150");
    expect(new Intl.Locale("und-AT").maximize().toString()).toBe("de-Latn-AT");
    expect(new Intl.Locale("und-Cyrl-RO").maximize().toString()).toBe("bg-Cyrl-RO");
    expect(new Intl.Locale("und-AQ").maximize().toString()).toBe("en-Latn-AQ");
});

test("keywords are preserved", () => {
    expect(new Intl.Locale("en-u-ca-abc").maximize().toString()).toBe("en-Latn-US-u-ca-abc");
    expect(new Intl.Locale("en", { calendar: "abc" }).maximize().toString()).toBe(
        "en-Latn-US-u-ca-abc"
    );
});
