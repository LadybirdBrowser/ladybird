test("basic functionality", () => {
    expect(WeakMap.prototype.delete).toHaveLength(1);

    var original = [
        [{ a: 1 }, 1],
        [{ a: 2 }, 2],
        [{ a: 3 }, 3],
        [Symbol("foo"), "bar"],
    ];
    const weakMap = new WeakMap(original);
    expect(weakMap.delete(original[0][0])).toBeTrue();
    expect(weakMap.delete(original[0][0])).toBeFalse();
    expect(weakMap.delete(original[3][0])).toBeTrue();
    expect(weakMap.delete(original[3][0])).toBeFalse();
    expect(weakMap.delete(null)).toBeFalse();
});
