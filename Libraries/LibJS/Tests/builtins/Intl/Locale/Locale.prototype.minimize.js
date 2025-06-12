test("length is 0", () => {
    expect(Intl.Locale.prototype.minimize).toHaveLength(0);
});

test("normal behavior", () => {
    expect(new Intl.Locale("en").minimize().toString()).toBe("en");

    expect(new Intl.Locale("en-Latn").minimize().toString()).toBe("en");
    expect(new Intl.Locale("ar-Arab").minimize().toString()).toBe("ar");
    expect(new Intl.Locale("en-US").minimize().toString()).toBe("en");
    expect(new Intl.Locale("en-GB").minimize().toString()).toBe("en-GB");

    expect(new Intl.Locale("en-Latn-US").minimize().toString()).toBe("en");
    expect(new Intl.Locale("en-Shaw-GB").minimize().toString()).toBe("en-Shaw");
    expect(new Intl.Locale("en-Arab-US").minimize().toString()).toBe("en-Arab");
    expect(new Intl.Locale("en-Latn-GB").minimize().toString()).toBe("en-GB");
    expect(new Intl.Locale("en-Latn-FR").minimize().toString()).toBe("en-FR");

    expect(new Intl.Locale("it-Kana-CA").minimize().toString()).toBe("it-Kana-CA");

    expect(new Intl.Locale("th-Thai-TH").minimize().toString()).toBe("th");
    expect(new Intl.Locale("es-Latn-419").minimize().toString()).toBe("es-419");
    expect(new Intl.Locale("ru-Cyrl-RU").minimize().toString()).toBe("ru");
    expect(new Intl.Locale("de-Latn-AT").minimize().toString()).toBe("de-AT");
    expect(new Intl.Locale("bg-Cyrl-RO").minimize().toString()).toBe("bg-RO");
    expect(new Intl.Locale("und-Latn-AQ").minimize().toString()).toBe("en-AQ");
});

test("keywords are preserved", () => {
    expect(new Intl.Locale("en-Latn-US-u-ca-abc").minimize().toString()).toBe("en-u-ca-abc");
    expect(new Intl.Locale("en-Latn-US", { calendar: "abc" }).minimize().toString()).toBe(
        "en-u-ca-abc"
    );
});
