test("length is 1", () => {
    expect(WeakSet.prototype.has).toHaveLength(1);
});

test("basic functionality", () => {
    var original = [{ a: 1 }, { a: 2 }, { a: 3 }, Symbol("foo")];
    var weakSet = new WeakSet(original);

    expect(new WeakSet().has()).toBeFalse();
    expect(weakSet.has(original[0])).toBeTrue();
    expect(weakSet.has(original[3])).toBeTrue();
    expect(weakSet.has({ a: 1 })).toBeFalse();
    expect(weakSet.has(Symbol("foo"))).toBeFalse();
});
