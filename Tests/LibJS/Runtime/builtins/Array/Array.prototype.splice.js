test("length is 2", () => {
    expect(Array.prototype.splice).toHaveLength(2);
});

test("basic functionality", () => {
    var array = ["hello", "friends", "serenity", 1, 2];
    var removed = array.splice(3);
    expect(array).toEqual(["hello", "friends", "serenity"]);
    expect(removed).toEqual([1, 2]);

    array = ["hello", "friends", "serenity", 1, 2];
    removed = array.splice(-2);
    expect(array).toEqual(["hello", "friends", "serenity"]);
    expect(removed).toEqual([1, 2]);

    array = ["hello", "friends", "serenity", 1, 2];
    removed = array.splice(-2, 1);
    expect(array).toEqual(["hello", "friends", "serenity", 2]);
    expect(removed).toEqual([1]);

    array = ["serenity"];
    removed = array.splice(0, 0, "hello", "friends");
    expect(array).toEqual(["hello", "friends", "serenity"]);
    expect(removed).toEqual([]);

    array = ["goodbye", "friends", "serenity"];
    removed = array.splice(0, 1, "hello");
    expect(array).toEqual(["hello", "friends", "serenity"]);
    expect(removed).toEqual(["goodbye"]);

    array = ["foo", "bar", "baz"];
    removed = array.splice();
    expect(array).toEqual(["foo", "bar", "baz"]);
    expect(removed).toEqual([]);

    removed = array.splice(0, 123);
    expect(array).toEqual([]);
    expect(removed).toEqual(["foo", "bar", "baz"]);

    array = ["foo", "bar", "baz"];
    removed = array.splice(123, 123);
    expect(array).toEqual(["foo", "bar", "baz"]);
    expect(removed).toEqual([]);

    array = ["foo", "bar", "baz"];
    removed = array.splice(-123, 123);
    expect(array).toEqual([]);
    expect(removed).toEqual(["foo", "bar", "baz"]);

    array = ["foo", "bar"];
    removed = array.splice(1, 1, "baz");
    expect(array).toEqual(["foo", "baz"]);
    expect(removed).toEqual(["bar"]);
});

test("Invalid lengths", () => {
    var length = Math.pow(2, 32);

    var obj = {
        length: length,
    };

    expect(() => {
        Array.prototype.splice.call(obj, 0);
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
    var removed = array.splice(1, 1, 9);
    expect(removed).toBeInstanceOf(ResultArray);
    expect(removed).toEqual([2]);
    expect(array).toEqual([1, 9, 3]);
});

test("throws if the array length is not writable", () => {
    var array = [1, 2, 3];
    Object.defineProperty(array, "length", { writable: false });

    expect(() => {
        array.splice(1, 1);
    }).toThrow(TypeError);
    expect(array[0]).toBe(1);
    expect(array[1]).toBe(3);
    expect(array[2]).toBeUndefined();
    expect(2 in array).toBeFalse();
    expect(array.length).toBe(3);
});
