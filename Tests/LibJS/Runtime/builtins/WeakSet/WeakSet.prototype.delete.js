test("basic functionality", () => {
    expect(WeakSet.prototype.delete).toHaveLength(1);

    var original = [{ a: 1 }, { a: 2 }, { a: 3 }, Symbol("foo")];
    const weakSet = new WeakSet(original);
    expect(weakSet.delete(original[0])).toBeTrue();
    expect(weakSet.delete(original[0])).toBeFalse();
    expect(weakSet.delete(original[3])).toBeTrue();
    expect(weakSet.delete(original[3])).toBeFalse();
    expect(weakSet.delete(null)).toBeFalse();
});
