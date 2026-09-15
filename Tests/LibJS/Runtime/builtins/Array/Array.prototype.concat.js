test("length is 1", () => {
    expect(Array.prototype.concat).toHaveLength(1);
});

describe("normal behavior", () => {
    var array = ["hello"];

    test("no arguments", () => {
        var concatenated = array.concat();
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(1);
    });

    test("single argument", () => {
        var concatenated = array.concat("friends");
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(2);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe("friends");
    });

    test("single array argument", () => {
        var concatenated = array.concat([1, 2, 3]);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(4);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe(1);
        expect(concatenated[2]).toBe(2);
        expect(concatenated[3]).toBe(3);
    });

    test("multiple arguments", () => {
        var concatenated = array.concat(false, "serenity", { name: "libjs" }, [1, [2, 3]]);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(6);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBeFalse();
        expect(concatenated[2]).toBe("serenity");
        expect(concatenated[3]).toEqual({ name: "libjs" });
        expect(concatenated[4]).toBe(1);
        expect(concatenated[5]).toEqual([2, 3]);
    });

    test("Proxy is concatenated as array", () => {
        var proxy = new Proxy([9, 8], {});
        var concatenated = array.concat(proxy);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(3);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe(9);
        expect(concatenated[2]).toBe(8);
    });
});

test("respects Symbol.isConcatSpreadable on packed array arguments", () => {
    var array = [1, 2];
    array[Symbol.isConcatSpreadable] = false;

    var concatenated = [0].concat(array);
    expect(concatenated).toEqual([0, array]);
});

test("uses ArraySpeciesCreate", () => {
    class ResultArray extends Array {}
    class DerivedArray extends Array {
        static get [Symbol.species]() {
            return ResultArray;
        }
    }

    var array = new DerivedArray(1, 2);
    var concatenated = array.concat([3]);
    expect(concatenated).toBeInstanceOf(ResultArray);
    expect(concatenated).toEqual([1, 2, 3]);
});

describe("empty arrays", () => {
    test("empty this and empty arguments", () => {
        expect([].concat()).toEqual([]);
        expect([].concat([])).toEqual([]);
        expect([].concat([], [], [])).toEqual([]);
        expect([].concat([1, 2], [], [3])).toEqual([1, 2, 3]);
        expect([1, 2].concat([])).toEqual([1, 2]);
    });

    test("result is a new array", () => {
        var empty = [];
        var result = empty.concat([]);
        expect(result).not.toBe(empty);
        result.push(1);
        expect(empty).toHaveLength(0);
    });

    test("reduce accumulating into an empty array", () => {
        var parts = [[], [1], [], [2, 3], []];
        expect(parts.reduce((all, part) => all.concat(part), [])).toEqual([1, 2, 3]);
    });

    test("empty array with own Symbol.isConcatSpreadable false", () => {
        var empty = [];
        empty[Symbol.isConcatSpreadable] = false;
        var result = [1].concat(empty);
        expect(result).toHaveLength(2);
        expect(result[1]).toBe(empty);

        var result2 = empty.concat([2]);
        expect(result2).toHaveLength(2);
        expect(result2[0]).toBe(empty);
        expect(result2[1]).toBe(2);
    });

    test("empty array with a non-default prototype", () => {
        var empty = [];
        Object.setPrototypeOf(empty, { [Symbol.isConcatSpreadable]: false, __proto__: Array.prototype });
        var result = [1].concat(empty);
        expect(result).toHaveLength(2);
        expect(result[1]).toBe(empty);
    });

    test("empty array whose length was set is spread with holes", () => {
        var sized = [];
        sized.length = 3;
        var result = [1].concat(sized, [2]);
        expect(result).toHaveLength(5);
        expect(1 in result).toBeFalse();
        expect(3 in result).toBeFalse();
        expect(result[4]).toBe(2);
    });
});

describe("non-Array arguments", () => {
    test("plain object is appended as one element", () => {
        var object = { 0: "a", length: 1 };
        var result = [1].concat(object, []);
        expect(result).toHaveLength(2);
        expect(result[1]).toBe(object);
    });

    test("array-like with Symbol.isConcatSpreadable is spread", () => {
        var arrayLike = { 0: "a", 2: "c", length: 3, [Symbol.isConcatSpreadable]: true };
        var result = [].concat(arrayLike);
        expect(result).toHaveLength(3);
        expect(result[0]).toBe("a");
        expect(1 in result).toBeFalse();
        expect(result[2]).toBe("c");
    });

    test("Symbol.isConcatSpreadable getter is called once per object", () => {
        var calls = 0;
        var object = {
            length: 1,
            0: "x",
            get [Symbol.isConcatSpreadable]() {
                ++calls;
                return true;
            },
        };
        expect([].concat(object, object)).toEqual(["x", "x"]);
        expect(calls).toBe(2);
    });

    test("Symbol.isConcatSpreadable getter on an array", () => {
        var calls = 0;
        var array = [1, 2];
        Object.defineProperty(array, Symbol.isConcatSpreadable, {
            get() {
                ++calls;
                return false;
            },
        });
        var result = [].concat(array);
        expect(calls).toBe(1);
        expect(result).toHaveLength(1);
        expect(result[0]).toBe(array);
    });

    test("typed array is appended as one element", () => {
        var typedArray = new Uint8Array([1, 2]);
        var result = [].concat(typedArray);
        expect(result).toHaveLength(1);
        expect(result[0]).toBe(typedArray);
    });

    test("spreadable typed array is spread", () => {
        var typedArray = new Uint8Array([1, 2]);
        typedArray[Symbol.isConcatSpreadable] = true;
        expect([].concat(typedArray)).toEqual([1, 2]);
    });

    test("arguments object is appended as one element", () => {
        var args = (function () {
            return arguments;
        })(1, 2);
        var result = [].concat(args);
        expect(result).toHaveLength(1);
        expect(result[0]).toBe(args);
    });

    test("proxy of an array is spread through its traps", () => {
        var log = [];
        var proxy = new Proxy([1, 2], {
            get(target, key, receiver) {
                log.push(typeof key === "symbol" ? key.toString() : key);
                return Reflect.get(target, key, receiver);
            },
        });
        expect([].concat(proxy)).toEqual([1, 2]);
        expect(log).toEqual(["Symbol(Symbol.isConcatSpreadable)", "length", "0", "1"]);
    });

    test("proxy of an empty array", () => {
        var proxy = new Proxy([], {});
        expect([1].concat(proxy)).toEqual([1]);
    });
});

describe("holes", () => {
    test("holes in this and arguments stay holes", () => {
        var result = [1, , 3].concat([, 5]);
        expect(result).toHaveLength(5);
        expect(1 in result).toBeFalse();
        expect(3 in result).toBeFalse();
        expect(result[4]).toBe(5);
    });

    test("holes read through to indexed properties on the prototype", () => {
        Array.prototype[1] = "from prototype";
        try {
            var result = [0, , 2].concat([], [, "b"]);
            expect(Object.hasOwn(result, 1)).toBeTrue();
            expect(result[1]).toBe("from prototype");
            expect(Object.hasOwn(result, 3)).toBeFalse();
            expect(result[4]).toBe("b");
            expect(result).toHaveLength(5);
            expect([].concat([]).length).toBe(0);
        } finally {
            delete Array.prototype[1];
        }
    });

    test("holes read through to indexed properties on Object.prototype", () => {
        Object.prototype[0] = "object prototype";
        try {
            var result = [].concat([, 1]);
            expect(Object.hasOwn(result, 0)).toBeTrue();
            expect(result[0]).toBe("object prototype");
        } finally {
            delete Object.prototype[0];
        }
    });
});

describe("Symbol.isConcatSpreadable on prototypes", () => {
    test("Array.prototype false makes arrays single elements", () => {
        Array.prototype[Symbol.isConcatSpreadable] = false;
        try {
            var a = [1];
            var b = [];
            var result = a.concat(b);
            expect(result).toHaveLength(2);
            expect(result[0]).toBe(a);
            expect(result[1]).toBe(b);
        } finally {
            delete Array.prototype[Symbol.isConcatSpreadable];
        }
        expect([1].concat([])).toEqual([1]);
    });

    test("Object.prototype true spreads array-likes", () => {
        Object.prototype[Symbol.isConcatSpreadable] = true;
        try {
            var result = [].concat({ length: 2, 0: "a", 1: "b" }, []);
            expect(result).toEqual(["a", "b"]);
        } finally {
            delete Object.prototype[Symbol.isConcatSpreadable];
        }
    });
});

describe("species", () => {
    test("species result that already has elements is overwritten", () => {
        var array = [7, 8];
        array.constructor = {
            [Symbol.species]: function () {
                return [1, 2, 3, 4, 5];
            },
        };
        var result = array.concat([9]);
        expect(result).toEqual([7, 8, 9]);
    });

    test("species result that is this", () => {
        var array = [];
        array.constructor = {
            [Symbol.species]: function () {
                return array;
            },
        };
        var result = array.concat([1, 2], [3]);
        expect(result).toBe(array);
        expect(result).toEqual([1, 2, 3]);
    });

    test("species result that is also an argument", () => {
        var other = [];
        var array = [1, 2];
        array.constructor = {
            [Symbol.species]: function () {
                return other;
            },
        };
        var result = array.concat(other, [3]);
        expect(result).toBe(other);
        expect(result).toEqual([1, 2, 1, 2, 3]);
    });

    test("frozen species result throws", () => {
        var array = [1];
        array.constructor = {
            [Symbol.species]: function () {
                return Object.freeze([]);
            },
        };
        expect(() => array.concat([2])).toThrow(TypeError);
    });

    test("non-extensible species result throws", () => {
        var array = [];
        array.constructor = {
            [Symbol.species]: function () {
                return Object.preventExtensions([]);
            },
        };
        expect(() => array.concat([1])).toThrow(TypeError);
    });

    test("subclass instance as argument is spread", () => {
        class MyArray extends Array {}
        var derived = MyArray.from([2, 3]);
        var result = [1].concat(derived, new MyArray());
        expect(result).not.toBeInstanceOf(MyArray);
        expect(result).toEqual([1, 2, 3]);
    });

    test("constructor getter runs once", () => {
        var calls = 0;
        var array = [1];
        Object.defineProperty(array, "constructor", {
            get() {
                ++calls;
                return Array;
            },
        });
        expect(array.concat([])).toEqual([1]);
        expect(calls).toBe(1);
    });
});

describe("large lengths", () => {
    test("large packed arrays", () => {
        var a = [];
        var b = [];
        for (var i = 0; i < 100000; ++i) {
            a.push(i);
            b.push(-i);
        }
        var result = [].concat(a, "middle", b);
        expect(result).toHaveLength(200001);
        expect(result[0]).toBe(0);
        expect(result[99999]).toBe(99999);
        expect(result[100000]).toBe("middle");
        expect(result[100001]).toBe(-0);
        expect(result[200000]).toBe(-99999);
    });

    test("array-like whose length exceeds the maximum throws", () => {
        var arrayLike = { length: 2 ** 53 - 1, [Symbol.isConcatSpreadable]: true };
        expect(() => [1].concat(arrayLike)).toThrowWithMessage(TypeError, "Maximum array size exceeded");
        expect(() => [].concat([], [1], arrayLike)).toThrowWithMessage(TypeError, "Maximum array size exceeded");
    });
});
