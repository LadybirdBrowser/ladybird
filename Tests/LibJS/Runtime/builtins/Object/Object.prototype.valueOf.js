test("basic functionality", () => {
    expect(Object.prototype.valueOf).toHaveLength(0);

    const o = {};
    expect(o.valueOf()).toBe(o);

    expect(Object.prototype.valueOf.call(42)).toEqual(new Number(42));
});
