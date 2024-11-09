test("length is 1", () => {
    expect(WeakMap.prototype.has).toHaveLength(1);
});

test("basic functionality", () => {
    var original = [
        [{ a: 1 }, 1],
        [{ a: 2 }, 2],
        [{ a: 3 }, 3],
        [Symbol("foo"), "bar"],
    ];
    var weakMap = new WeakMap(original);

    expect(new WeakMap().has()).toBeFalse();
    expect(weakMap.has(original[0][0])).toBeTrue();
    expect(weakMap.has(original[3][0])).toBeTrue();
    expect(weakMap.has({ a: 1 })).toBeFalse();
});
