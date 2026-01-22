test("basic functionality", () => {
    expect(String.prototype.toUpperCase).toHaveLength(0);

    expect("ω".toUpperCase()).toBe("Ω");
    expect("Ω".toUpperCase()).toBe("Ω");
    expect("😀".toUpperCase()).toBe("😀");

    expect("foo".toUpperCase()).toBe("FOO");
    expect("Foo".toUpperCase()).toBe("FOO");
    expect("FOO".toUpperCase()).toBe("FOO");

    expect(("b" + "a" + +"n" + "a").toUpperCase()).toBe("BANANA");
});

test("special case folding", () => {
    expect("\u00DF".toUpperCase()).toBe("\u0053\u0053");
    expect("\u0130".toUpperCase()).toBe("\u0130");
    expect("\uFB00".toUpperCase()).toBe("\u0046\u0046");
    expect("\uFB01".toUpperCase()).toBe("\u0046\u0049");
    expect("\uFB02".toUpperCase()).toBe("\u0046\u004C");
    expect("\uFB03".toUpperCase()).toBe("\u0046\u0046\u0049");
    expect("\uFB04".toUpperCase()).toBe("\u0046\u0046\u004C");
    expect("\uFB05".toUpperCase()).toBe("\u0053\u0054");
    expect("\uFB06".toUpperCase()).toBe("\u0053\u0054");
    expect("\u0390".toUpperCase()).toBe("\u0399\u0308\u0301");
    expect("\u03B0".toUpperCase()).toBe("\u03A5\u0308\u0301");
    expect("\u1FB7".toUpperCase()).toBe("\u0391\u0342\u0399");
    expect("\u1FC7".toUpperCase()).toBe("\u0397\u0342\u0399");
    expect("\u1FF7".toUpperCase()).toBe("\u03A9\u0342\u0399");
});
