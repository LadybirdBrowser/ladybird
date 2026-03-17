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
