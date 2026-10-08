// The iteration methods of Array.prototype are written in JavaScript; these check what that must not change.

describe("observable steps", () => {
    test("proxy receivers see HasProperty before Get for every present index", () => {
        const log = [];
        const target = [10, , 30];
        const proxy = new Proxy(target, {
            has(target, key) {
                log.push(`has ${String(key)}`);
                return Reflect.has(target, key);
            },
            get(target, key, receiver) {
                log.push(`get ${String(key)}`);
                return Reflect.get(target, key, receiver);
            },
        });
        const seen = [];
        Array.prototype.forEach.call(proxy, value => seen.push(value));
        expect(seen).toEqual([10, 30]);
        expect(log).toEqual(["get length", "has 0", "get 0", "has 1", "has 2", "get 2"]);
    });

    test("find visits holes through Get without HasProperty", () => {
        const log = [];
        const proxy = new Proxy([1, , 3], {
            has(target, key) {
                log.push(`has ${String(key)}`);
                return Reflect.has(target, key);
            },
            get(target, key, receiver) {
                log.push(`get ${String(key)}`);
                return Reflect.get(target, key, receiver);
            },
        });
        expect(Array.prototype.find.call(proxy, () => false)).toBeUndefined();
        expect(log).toEqual(["get length", "get 0", "get 1", "get 2"]);
    });

    test("the length is read once, before the callback check", () => {
        let lengthReads = 0;
        const arrayLike = {
            get length() {
                ++lengthReads;
                return 2;
            },
            0: "a",
            1: "b",
        };
        expect(() => Array.prototype.map.call(arrayLike, 1)).toThrowWithMessage(TypeError, "1 is not a function");
        expect(lengthReads).toBe(1);
        expect(Array.prototype.map.call(arrayLike, x => x + x)).toEqual(["aa", "bb"]);
        expect(lengthReads).toBe(2);
    });

    test("elements appended during iteration are not visited, deleted ones are skipped", () => {
        const array = [1, 2, 3];
        const seen = [];
        array.forEach((value, index) => {
            seen.push(value);
            if (index === 0) {
                array.push(4);
                delete array[1];
            }
        });
        expect(seen).toEqual([1, 3]);
    });

    test("getters on indices run once per visit", () => {
        let reads = 0;
        const array = [1, 2];
        Object.defineProperty(array, 1, {
            get() {
                ++reads;
                return 20;
            },
        });
        expect(array.map(x => x)).toEqual([1, 20]);
        expect(reads).toBe(1);
    });

    test("primitive receivers are converted to objects", () => {
        expect(Array.prototype.map.call("ab", (c, i, object) => typeof object + c + i)).toEqual([
            "objecta0",
            "objectb1",
        ]);
        expect(() => Array.prototype.forEach.call(null, () => {})).toThrow(TypeError);
    });
});

describe("arguments", () => {
    test("thisArg is passed to the callback as is", () => {
        const thisArg = {};
        [1].forEach(function () {
            "use strict";
            expect(this).toBe(thisArg);
        }, thisArg);
        [1].forEach(function () {
            "use strict";
            expect(this).toBeUndefined();
        });
        [1].some(function () {
            "use strict";
            expect(this).toBe(42);
        }, 42);
    });

    test("an explicitly passed undefined is an initial value", () => {
        expect([].reduce(() => 1, undefined)).toBeUndefined();
        expect([].reduceRight(() => 1, undefined)).toBeUndefined();
        expect([1, 2].reduce((a, b) => [a, b], undefined)).toEqual([[undefined, 1], 2]);
        expect(() => [].reduce(() => 1)).toThrowWithMessage(TypeError, "Reduce of empty array with no initial value");
        expect(() => [, ,].reduceRight(() => 1)).toThrowWithMessage(
            TypeError,
            "Reduce of empty array with no initial value"
        );
    });

    test("the callback gets the value, the index and the object", () => {
        const array = ["x"];
        array.forEach((...args) => expect(args).toEqual(["x", 0, array]));
        array.reduce((...args) => expect(args).toEqual(["init", "x", 0, array]), "init");
    });
});

describe("species", () => {
    test("map and filter create the species of the receiver", () => {
        class MyArray extends Array {}
        const array = MyArray.from([1, 2, 3]);
        expect(array.map(x => x)).toBeInstanceOf(MyArray);
        expect(array.filter(x => x > 1)).toBeInstanceOf(MyArray);
        expect(array.filter(x => x > 1)).toEqual(MyArray.from([2, 3]));
    });
});

describe("stack traces", () => {
    test("frames of builtins have no source position", () => {
        let stack;
        [1].forEach(() => {
            stack = new Error().stack;
        });
        expect(stack.includes("at forEach\n")).toBeTrue();
        expect(stack.includes("BuiltinFile")).toBeFalse();
    });

    test("deep recursion through builtins works", () => {
        function recurse(depth) {
            return depth === 0 ? 0 : [depth].map(value => recurse(value - 1) + 1)[0];
        }
        expect(recurse(200)).toBe(200);
    });

    test("unbounded recursion through builtins throws", () => {
        function recurse() {
            [0].forEach(recurse);
        }
        expect(recurse).toThrow();
    });

    test("exceptions from callbacks propagate through builtins and can be caught", () => {
        let caught = null;
        try {
            [1, 2].map(value => {
                if (value === 2) throw new Error("callback");
                return value;
            });
        } catch (error) {
            caught = error.message;
        }
        expect(caught).toBe("callback");
        expect([1, 2].map(value => value * 2)).toEqual([2, 4]);
    });
});
