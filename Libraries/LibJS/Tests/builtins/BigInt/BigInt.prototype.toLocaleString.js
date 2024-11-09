test("basic functionality", () => {
    expect(BigInt.prototype.toLocaleString).toHaveLength(0);
    expect(BigInt(123).toLocaleString()).toBe("123");
});

test("calling with non-BigInt |this|", () => {
    expect(() => {
        BigInt.prototype.toLocaleString.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type BigInt");
});

test("default", () => {
    expect(1n.toLocaleString("en")).toBe("1");
    expect(12n.toLocaleString("en")).toBe("12");
    expect(123n.toLocaleString("en")).toBe("123");
    expect(123456789123456789123456789123456789n.toLocaleString("en")).toBe(
        "123,456,789,123,456,789,123,456,789,123,456,789"
    );

    const ar = new Intl.NumberFormat("ar");
    expect(1n.toLocaleString("ar")).toBe("\u0661");
    expect(12n.toLocaleString("ar")).toBe("\u0661\u0662");
    expect(123n.toLocaleString("ar")).toBe("\u0661\u0662\u0663");
    expect(123456789123456789123456789123456789n.toLocaleString("ar")).toBe(
        "\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669\u066c\u0661\u0662\u0663\u066c\u0664\u0665\u0666\u066c\u0667\u0668\u0669"
    );
});

test("integer digits", () => {
    expect(1n.toLocaleString("en", { minimumIntegerDigits: 2 })).toBe("01");
    expect(12n.toLocaleString("en", { minimumIntegerDigits: 2 })).toBe("12");
    expect(123n.toLocaleString("en", { minimumIntegerDigits: 2 })).toBe("123");

    expect(1n.toLocaleString("ar", { minimumIntegerDigits: 2 })).toBe("\u0660\u0661");
    expect(12n.toLocaleString("ar", { minimumIntegerDigits: 2 })).toBe("\u0661\u0662");
    expect(123n.toLocaleString("ar", { minimumIntegerDigits: 2 })).toBe("\u0661\u0662\u0663");
});

test("significant digits", () => {
    expect(
        1n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("1.000");
    expect(
        12n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("12.00");
    expect(
        123n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("123.0");
    expect(
        1234n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("1,234");
    expect(
        12345n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("12,345");
    expect(
        123456n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("123,456");
    expect(
        1234567n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("1,234,570");
    expect(
        1234561n.toLocaleString("en", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("1,234,560");

    expect(
        1n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u066b\u0660\u0660\u0660");
    expect(
        12n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u0662\u066b\u0660\u0660");
    expect(
        123n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u0662\u0663\u066b\u0660");
    expect(
        1234n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u066c\u0662\u0663\u0664");
    expect(
        12345n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u0662\u066c\u0663\u0664\u0665");
    expect(
        123456n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u0662\u0663\u066c\u0664\u0665\u0666");
    expect(
        1234567n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0667\u0660");
    expect(
        1234561n.toLocaleString("ar", { minimumSignificantDigits: 4, maximumSignificantDigits: 6 })
    ).toBe("\u0661\u066c\u0662\u0663\u0664\u066c\u0665\u0666\u0660");
});
