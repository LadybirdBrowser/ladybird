test("adding strings", () => {
    expect("" + "").toBe("");
    expect("ab" + "").toBe("ab");
    expect("" + "cd").toBe("cd");
    expect("ab" + "cd").toBe("abcd");
});

test("adding strings with non-strings", () => {
    expect("a" + 1).toBe("a1");
    expect(1 + "a").toBe("1a");
    expect("a" + {}).toBe("a[object Object]");
    expect({} + "a").toBeNaN();
    expect("a" + []).toBe("a");
    expect([] + "a").toBe("a");
    expect("a" + NaN).toBe("aNaN");
    expect(NaN + "a").toBe("NaNa");
    expect(Array(16).join([[][[]] + []][+[]][++[+[]][+[]]] - 1) + " Batman!").toBe(
        "NaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaN Batman!"
    );
});

test("adding strings with dangling surrogates", () => {
    expect("\ud834" + "").toBe("\ud834");
    expect("" + "\udf06").toBe("\udf06");
    expect("\ud834" + "\udf06").toBe("𝌆");
    expect("\ud834" + "\ud834").toBe("\ud834\ud834");
    expect("\udf06" + "\udf06").toBe("\udf06\udf06");
    expect("\ud834a" + "\udf06").toBe("\ud834a\udf06");
    expect("\ud834" + "a\udf06").toBe("\ud834a\udf06");
});
