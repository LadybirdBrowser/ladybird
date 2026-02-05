test("length is 0", () => {
    expect(Array.prototype.pop).toHaveLength(0);
});

describe("normal behavior", () => {
    test("array with elements", () => {
        var a = [1, 2, 3];
        expect(a.pop()).toBe(3);
        expect(a).toEqual([1, 2]);
        expect(a.pop()).toBe(2);
        expect(a).toEqual([1]);
        expect(a.pop()).toBe(1);
        expect(a).toEqual([]);
        expect(a.pop()).toBeUndefined();
        expect(a).toEqual([]);
    });

    test("empty array", () => {
        var a = [];
        expect(a.pop()).toBeUndefined();
        expect(a).toEqual([]);
    });

    test("array with empty slot", () => {
        var a = [,];
        expect(a.pop()).toBeUndefined();
        expect(a).toEqual([]);
    });

    test("array with prototype indexed value", () => {
        Array.prototype[1] = 1;

        var a = [0];
        a.length = 2;
        expect(a[1]).toEqual(1);
        expect(a.pop()).toEqual(1);

        expect(a.length).toEqual(1);
        expect(a).toEqual([0]);
        expect(a[1]).toEqual(1);

        delete Array.prototype[1];
    });
});
