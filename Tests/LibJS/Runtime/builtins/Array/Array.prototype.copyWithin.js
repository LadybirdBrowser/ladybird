test("length is 2", () => {
    expect(Array.prototype.copyWithin).toHaveLength(2);
});

describe("normal behavior", () => {
    test("Noop", () => {
        var array = [1, 2];
        array.copyWithin(0, 0);
        expect(array).toEqual([1, 2]);
    });

    test("basic behavior", () => {
        var array = [1, 2, 3];

        var b = array.copyWithin(1, 2);
        expect(b).toEqual(array);
        expect(array).toEqual([1, 3, 3]);

        b = array.copyWithin(2, 0);
        expect(b).toEqual(array);
        expect(array).toEqual([1, 3, 1]);
    });

    test("start > target", () => {
        var array = [1, 2, 3];
        var b = array.copyWithin(0, 1);
        expect(b).toEqual(array);
        expect(array).toEqual([2, 3, 3]);
    });

    test("overwriting behavior", () => {
        var array = [1, 2, 3];
        var b = array.copyWithin(1, 0);
        expect(b).toEqual(array);
        expect(array).toEqual([1, 1, 2]);
    });

    test("specify end", () => {
        var array = [1, 2, 3];

        b = array.copyWithin(2, 0, 1);
        expect(b).toEqual(array);
        expect(array).toEqual([1, 2, 1]);
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].copyWithin).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            copyWithin;
        }).toThrowWithMessage(ReferenceError, "'copyWithin' is not defined");
    }
});
