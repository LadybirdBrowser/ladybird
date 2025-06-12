test("length", () => {
    expect(Map.prototype.entries.length).toBe(0);
});

test("basic functionality", () => {
    const original = [
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ];
    const a = new Map(original);
    const it = a.entries();
    expect(it.next()).toEqual({ value: ["a", 0], done: false });
    expect(it.next()).toEqual({ value: ["b", 1], done: false });
    expect(it.next()).toEqual({ value: ["c", 2], done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});
