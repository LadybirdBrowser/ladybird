test("length is 1", () => {
    expect(Array.prototype.fill).toHaveLength(1);
});

test("basic functionality", () => {
    var array = [1, 2, 3, 4];

    expect(array.fill(0, 2, 4)).toEqual([1, 2, 0, 0]);
    expect(array.fill(5, 1)).toEqual([1, 5, 5, 5]);
    expect(array.fill(6)).toEqual([6, 6, 6, 6]);

    expect([1, 2, 3].fill(4)).toEqual([4, 4, 4]);
    expect([1, 2, 3].fill(4, 1)).toEqual([1, 4, 4]);
    expect([1, 2, 3].fill(4, 1, 2)).toEqual([1, 4, 3]);
    expect([1, 2, 3].fill(4, 3, 3)).toEqual([1, 2, 3]);
    expect([1, 2, 3].fill(4, -3, -2)).toEqual([4, 2, 3]);
    expect([1, 2, 3].fill(4, NaN, NaN)).toEqual([1, 2, 3]);
    expect([1, 2, 3].fill(4, 3, 5)).toEqual([1, 2, 3]);
    expect(Array(3).fill(4)).toEqual([4, 4, 4]);
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].fill).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            fill;
        }).toThrowWithMessage(ReferenceError, "'fill' is not defined");
    }
});
