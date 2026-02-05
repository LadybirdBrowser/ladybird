test("length is 0", () => {
    expect(Intl.Locale.prototype.toString).toHaveLength(0);
});

test("normal behavior", () => {
    const en1 = new Intl.Locale("en");
    expect(en1.toString()).toBe("en");

    const en2 = new Intl.Locale("en-Latn");
    expect(en2.toString()).toBe("en-Latn");

    const en3 = new Intl.Locale("en-US");
    expect(en3.toString()).toBe("en-US");

    const en4 = new Intl.Locale("en", { language: "es" });
    expect(en4.toString()).toBe("es");

    const en5 = new Intl.Locale("en", { script: "Latn" });
    expect(en5.toString()).toBe("en-Latn");

    const en6 = new Intl.Locale("en", { script: "Latn", region: "US" });
    expect(en6.toString()).toBe("en-Latn-US");
});

test("string is canonicalized behavior", () => {
    const en = new Intl.Locale("EN", { script: "lAtN", region: "us" });
    expect(en.toString()).toBe("en-Latn-US");
});
