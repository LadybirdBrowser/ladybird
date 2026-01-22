test("length", () => {
    expect(Map.prototype.keys.length).toBe(0);
});

test("basic functionality", () => {
    const original = [
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ];
    const a = new Map(original);
    const it = a.keys();
    expect(it.next()).toEqual({ value: "a", done: false });
    expect(it.next()).toEqual({ value: "b", done: false });
    expect(it.next()).toEqual({ value: "c", done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});
