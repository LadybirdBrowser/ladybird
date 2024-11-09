test("constructor properties", () => {
    expect(String).toHaveLength(1);
    expect(String.name).toBe("String");
});

test("typeof", () => {
    expect(typeof String()).toBe("string");
    expect(typeof new String()).toBe("object");
});

test("length", () => {
    expect(new String().length).toBe(0);
    expect(new String("a").length).toBe(1);
    expect(new String("\u180E").length).toBe(1);
    expect(new String("\uDBFF\uDFFF").length).toBe(2);

    // Issue #2280
    expect("⛳".length).toBe(1);
    expect("🔥".length).toBe(2);
    expect("🔥🔥🔥".length).toBe(6);
    expect("👨‍👩‍👦".length).toBe(8);
    expect("👩‍❤️‍💋‍👩".length).toBe(11);
});

test("indices", () => {
    expect("abc"[0]).toBe("a");
    expect("abc"[1]).toBe("b");
    expect("abc"[2]).toBe("c");
    expect("abc"[3]).toBeUndefined();

    expect("😀"[0]).toBe("\ud83d");
    expect("😀"[1]).toBe("\ude00");
    expect("😀"[2]).toBeUndefined();
});

test("properties", () => {
    expect(Object.getOwnPropertyNames("")).toEqual(["length"]);
    expect(Object.getOwnPropertyNames("a")).toEqual(["0", "length"]);
    expect(Object.getOwnPropertyNames("ab")).toEqual(["0", "1", "length"]);
    expect(Object.getOwnPropertyNames("abc")).toEqual(["0", "1", "2", "length"]);
    expect(Object.getOwnPropertyNames("😀")).toEqual(["0", "1", "length"]);
});
