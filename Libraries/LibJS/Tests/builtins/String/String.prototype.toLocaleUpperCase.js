test("basic functionality", () => {
    expect(String.prototype.toLocaleUpperCase).toHaveLength(0);

    expect("ω".toLocaleUpperCase()).toBe("Ω");
    expect("Ω".toLocaleUpperCase()).toBe("Ω");
    expect("😀".toLocaleUpperCase()).toBe("😀");

    expect("foo".toLocaleUpperCase()).toBe("FOO");
    expect("Foo".toLocaleUpperCase()).toBe("FOO");
    expect("FOO".toLocaleUpperCase()).toBe("FOO");

    expect(("b" + "a" + +"n" + "a").toLocaleUpperCase()).toBe("BANANA");
});

test("special case folding", () => {
    expect("\u00DF".toLocaleUpperCase()).toBe("\u0053\u0053");
    expect("\u0130".toLocaleUpperCase()).toBe("\u0130");
    expect("\uFB00".toLocaleUpperCase()).toBe("\u0046\u0046");
    expect("\uFB01".toLocaleUpperCase()).toBe("\u0046\u0049");
    expect("\uFB02".toLocaleUpperCase()).toBe("\u0046\u004C");
    expect("\uFB03".toLocaleUpperCase()).toBe("\u0046\u0046\u0049");
    expect("\uFB04".toLocaleUpperCase()).toBe("\u0046\u0046\u004C");
    expect("\uFB05".toLocaleUpperCase()).toBe("\u0053\u0054");
    expect("\uFB06".toLocaleUpperCase()).toBe("\u0053\u0054");
    expect("\u0390".toLocaleUpperCase()).toBe("\u0399\u0308\u0301");
    expect("\u03B0".toLocaleUpperCase()).toBe("\u03A5\u0308\u0301");
    expect("\u1FB7".toLocaleUpperCase()).toBe("\u0391\u0342\u0399");
    expect("\u1FC7".toLocaleUpperCase()).toBe("\u0397\u0342\u0399");
    expect("\u1FF7".toLocaleUpperCase()).toBe("\u03A9\u0342\u0399");

    expect("i".toLocaleUpperCase()).toBe("I");
    expect("i".toLocaleUpperCase("lt")).toBe("I");

    expect("i\u0307".toLocaleUpperCase()).toBe("I\u0307");
    expect("i\u0307".toLocaleUpperCase("lt")).toBe("I");

    expect("j".toLocaleUpperCase()).toBe("J");
    expect("j".toLocaleUpperCase("lt")).toBe("J");

    expect("j\u0307".toLocaleUpperCase()).toBe("J\u0307");
    expect("j\u0307".toLocaleUpperCase("lt")).toBe("J");

    expect("j\u0307".toLocaleUpperCase(["en", "lt"])).toBe("J\u0307");
    expect("j\u0307".toLocaleUpperCase(["lt", "en"])).toBe("J");
});
