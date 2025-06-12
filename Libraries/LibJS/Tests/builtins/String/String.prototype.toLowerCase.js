test("basic functionality", () => {
    expect(String.prototype.toLowerCase).toHaveLength(0);

    expect("ω".toLowerCase()).toBe("ω");
    expect("Ω".toLowerCase()).toBe("ω");
    expect("😀".toLowerCase()).toBe("😀");

    expect("foo".toLowerCase()).toBe("foo");
    expect("Foo".toLowerCase()).toBe("foo");
    expect("FOO".toLowerCase()).toBe("foo");

    expect(("b" + "a" + +"a" + "a").toLowerCase()).toBe("banana");
});

test("special case folding", () => {
    expect("\u00DF".toLowerCase()).toBe("\u00DF");
    expect("\u0130".toLowerCase()).toBe("\u0069\u0307");
    expect("\uFB00".toLowerCase()).toBe("\uFB00");
    expect("\uFB01".toLowerCase()).toBe("\uFB01");
    expect("\uFB02".toLowerCase()).toBe("\uFB02");
    expect("\uFB03".toLowerCase()).toBe("\uFB03");
    expect("\uFB04".toLowerCase()).toBe("\uFB04");
    expect("\uFB05".toLowerCase()).toBe("\uFB05");
    expect("\uFB06".toLowerCase()).toBe("\uFB06");
    expect("\u1FB7".toLowerCase()).toBe("\u1FB7");
    expect("\u1FC7".toLowerCase()).toBe("\u1FC7");
    expect("\u1FF7".toLowerCase()).toBe("\u1FF7");
});
