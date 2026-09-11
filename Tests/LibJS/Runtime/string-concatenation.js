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

test("length of concatenated strings", () => {
    expect(("item " + 1023).length).toBe(9);
    expect(("\ud834" + "\udf06" + "abcdefgh").length).toBe(10);

    const source = "abcdefghijklmnop";
    expect((source.slice(3) + source.slice(0, 5) + "xyz").length).toBe(21);

    const left = "abcdefgh" + "ijklmnop";
    const right = "qrstuvwx" + "yz";
    expect((left + right).length).toBe(26);
    expect(left + right).toBe("abcdefghijklmnopqrstuvwxyz");

    let string = "";
    for (let i = 0; i < 1000; i++) {
        string += "ab";
        expect(string.length).toBe((i + 1) * 2);
    }
    expect(string).toBe("ab".repeat(1000));
});
