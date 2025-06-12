test("length is 0", () => {
    expect(Array.prototype.reverse).toHaveLength(0);
});

describe("basic functionality", () => {
    test("Odd length array", () => {
        var array = [1, 2, 3];
        expect(array.reverse()).toEqual([3, 2, 1]);
        expect(array).toEqual([3, 2, 1]);
    });

    test("Even length array", () => {
        var array = [1, 2];
        expect(array.reverse()).toEqual([2, 1]);
        expect(array).toEqual([2, 1]);
    });

    test("Empty array", () => {
        var array = [];
        expect(array.reverse()).toEqual([]);
        expect(array).toEqual([]);
    });
});
