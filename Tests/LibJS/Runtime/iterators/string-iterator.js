test("length", () => {
    expect(String.prototype[Symbol.iterator]).toHaveLength(0);
});

test("basic functionality", () => {
    const s = "abcd";
    const it = s[Symbol.iterator]();
    expect(it.next()).toEqual({ value: "a", done: false });
    expect(it.next()).toEqual({ value: "b", done: false });
    expect(it.next()).toEqual({ value: "c", done: false });
    expect(it.next()).toEqual({ value: "d", done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("casts |this| to string", () => {
    let it = String.prototype[Symbol.iterator].call(45);
    expect(it.next()).toEqual({ value: "4", done: false });
    expect(it.next()).toEqual({ value: "5", done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });

    it = String.prototype[Symbol.iterator].call(false);
    expect(it.next()).toEqual({ value: "f", done: false });
    expect(it.next()).toEqual({ value: "a", done: false });
    expect(it.next()).toEqual({ value: "l", done: false });
    expect(it.next()).toEqual({ value: "s", done: false });
    expect(it.next()).toEqual({ value: "e", done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });

    expect(() => {
        String.prototype[Symbol.iterator].call(null);
    }).toThrowWithMessage(TypeError, "null cannot be converted to an object");
    expect(() => {
        String.prototype[Symbol.iterator].call(undefined);
    }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");
});

test("utf8 compatible", () => {
    const it = "ab\u{1f41e}cde"[Symbol.iterator]();
    expect(it.next()).toEqual({ value: "a", done: false });
    expect(it.next()).toEqual({ value: "b", done: false });
    expect(it.next()).toEqual({ value: "🐞", done: false });
    expect(it.next()).toEqual({ value: "c", done: false });
    expect(it.next()).toEqual({ value: "d", done: false });
    expect(it.next()).toEqual({ value: "e", done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
});
