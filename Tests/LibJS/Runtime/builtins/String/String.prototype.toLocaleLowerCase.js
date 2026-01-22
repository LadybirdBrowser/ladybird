test("basic functionality", () => {
    expect(String.prototype.toLocaleLowerCase).toHaveLength(0);

    expect("ω".toLocaleLowerCase()).toBe("ω");
    expect("Ω".toLocaleLowerCase()).toBe("ω");
    expect("😀".toLocaleLowerCase()).toBe("😀");

    expect("foo".toLocaleLowerCase()).toBe("foo");
    expect("Foo".toLocaleLowerCase()).toBe("foo");
    expect("FOO".toLocaleLowerCase()).toBe("foo");

    expect(("b" + "a" + +"a" + "a").toLocaleLowerCase()).toBe("banana");
});

test("special case folding", () => {
    expect("\u00DF".toLocaleLowerCase()).toBe("\u00DF");
    expect("\u0130".toLocaleLowerCase()).toBe("\u0069\u0307");
    expect("\uFB00".toLocaleLowerCase()).toBe("\uFB00");
    expect("\uFB01".toLocaleLowerCase()).toBe("\uFB01");
    expect("\uFB02".toLocaleLowerCase()).toBe("\uFB02");
    expect("\uFB03".toLocaleLowerCase()).toBe("\uFB03");
    expect("\uFB04".toLocaleLowerCase()).toBe("\uFB04");
    expect("\uFB05".toLocaleLowerCase()).toBe("\uFB05");
    expect("\uFB06".toLocaleLowerCase()).toBe("\uFB06");
    expect("\u1FB7".toLocaleLowerCase()).toBe("\u1FB7");
    expect("\u1FC7".toLocaleLowerCase()).toBe("\u1FC7");
    expect("\u1FF7".toLocaleLowerCase()).toBe("\u1FF7");

    expect("I".toLocaleLowerCase()).toBe("i");
    expect("I".toLocaleLowerCase("az")).toBe("\u0131");
    expect("I".toLocaleLowerCase("tr")).toBe("\u0131");

    expect("\u0130".toLocaleLowerCase()).toBe("\u0069\u0307");
    expect("\u0130".toLocaleLowerCase("az")).toBe("i");
    expect("\u0130".toLocaleLowerCase("tr")).toBe("i");

    expect("I\u0307".toLocaleLowerCase()).toBe("i\u0307");
    expect("I\u0307".toLocaleLowerCase("az")).toBe("i");
    expect("I\u0307".toLocaleLowerCase("tr")).toBe("i");

    expect("\u012e".toLocaleLowerCase()).toBe("\u012f");
    expect("\u012e".toLocaleLowerCase("lt")).toBe("\u012f");

    expect("\u012e\u0300".toLocaleLowerCase()).toBe("\u012f\u0300");
    expect("\u012e\u0300".toLocaleLowerCase("lt")).toBe("\u012f\u0307\u0300");

    expect("\u012e\u0300".toLocaleLowerCase(["en", "lt"])).toBe("\u012f\u0300");
    expect("\u012e\u0300".toLocaleLowerCase(["lt", "en"])).toBe("\u012f\u0307\u0300");
});
