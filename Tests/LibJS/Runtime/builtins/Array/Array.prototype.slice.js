test("length is 0", () => {
    expect(Array.prototype.slice).toHaveLength(2);
});

test("basic functionality", () => {
    var array = ["hello", "friends", "serenity", 1];

    var slice = array.slice();
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["hello", "friends", "serenity", 1]);

    slice = array.slice(1);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["friends", "serenity", 1]);

    slice = array.slice(0, 2);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["hello", "friends"]);

    slice = array.slice(-1);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual([1]);

    slice = array.slice(1, 1);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual([]);

    slice = array.slice(1, -1);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["friends", "serenity"]);

    slice = array.slice(2, -1);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["serenity"]);

    slice = array.slice(0, 100);
    expect(array).toEqual(["hello", "friends", "serenity", 1]);
    expect(slice).toEqual(["hello", "friends", "serenity", 1]);
});

test("Invalid lengths", () => {
    var length = Math.pow(2, 32);

    var obj = {
        length: length,
    };

    expect(() => {
        Array.prototype.slice.call(obj, 0);
    }).toThrowWithMessage(RangeError, "Invalid array length");
});

test("uses ArraySpeciesCreate", () => {
    class ResultArray extends Array {}
    class DerivedArray extends Array {
        static get [Symbol.species]() {
            return ResultArray;
        }
    }

    var array = new DerivedArray(1, 2, 3);
    var slice = array.slice(1);
    expect(slice).toBeInstanceOf(ResultArray);
    expect(slice).toEqual([2, 3]);
});

describe("species result that already has elements", () => {
    test("longer packed result is truncated", () => {
        var array = [1, 2, 3, 4, 5];
        array.constructor = {
            [Symbol.species]: function () {
                return [9, 9, 9, 9, 9, 9];
            },
        };
        var slice = array.slice(0, 2);
        expect(slice).toEqual([1, 2]);
        expect(slice).toHaveLength(2);
    });

    test("bounds prefilled species results across optimized and generic paths", () => {
        const sliced = extensible => {
            const source = ["allowed"];
            const result = ["sentinel", "protected-tail"];
            if (!extensible) Object.preventExtensions(result);
            source.constructor = {
                [Symbol.species]: function () {
                    return result;
                },
            };
            return source.slice(0, 1);
        };

        expect(sliced(true)).toEqual(["allowed"]);
        expect(sliced(false)).toEqual(["allowed"]);
    });

    test("longer holey result is truncated", () => {
        var array = [1, 2, 3];
        array.constructor = {
            [Symbol.species]: function () {
                return [9, , 9, 9];
            },
        };
        var slice = array.slice(2);
        expect(slice).toEqual([3]);
    });

    test("empty slice truncates the result to zero", () => {
        var array = [1, 2, 3];
        array.constructor = {
            [Symbol.species]: function () {
                return [9, 9];
            },
        };
        expect(array.slice(2, 1)).toHaveLength(0);
    });

    test("result that is this", () => {
        var array = [1, 2, 3, 4, 5];
        array.constructor = {
            [Symbol.species]: function () {
                return array;
            },
        };
        var slice = array.slice(1, 3);
        expect(slice).toBe(array);
        expect(array).toEqual([2, 3]);
    });
});
