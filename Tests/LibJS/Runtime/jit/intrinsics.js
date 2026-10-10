// JIT code handles some builtins and operators inline, or through the runtime without a native frame: hasOwnProperty
// (also through Function.prototype.call), `in`, charCodeAt, charAt, String.fromCharCode and calls of the String, Array,
// Object and Boolean constructors. These tests run each site often enough to be compiled with a low JIT threshold,
// then pass it what the inline paths do not handle.

const iterations = 100;

describe("Object.prototype.hasOwnProperty", () => {
    test("own, inherited, deleted and index keys", () => {
        function has(object, key) {
            return object.hasOwnProperty(key);
        }
        const object = { a: 1, b: undefined };
        const child = Object.create(object);
        child.c = 3;
        for (let i = 0; i < iterations; ++i) {
            expect(has(object, "a")).toBeTrue();
            expect(has(object, "b")).toBeTrue();
            expect(has(object, "c")).toBeFalse();
            expect(has(child, "a")).toBeFalse();
            expect(has(child, "c")).toBeTrue();
        }
        delete object.a;
        expect(has(object, "a")).toBeFalse();
        expect(has([1, 2], "1")).toBeTrue();
        expect(has([1, 2], "2")).toBeFalse();
        expect(has([1, 2], "length")).toBeTrue();
        expect(has({ 0: "x" }, "0")).toBeTrue();
        expect(has({ 0: "x" }, 0)).toBeTrue();
        expect(has(new String("ab"), "1")).toBeTrue();
        const symbol = Symbol("s");
        expect(has({ [symbol]: 1 }, symbol)).toBeTrue();
        expect(has({ toString: 1 }, { toString: () => "toString" })).toBeTrue();
        expect(has("abc", "length")).toBeTrue();
        expect(() => has(null, "a")).toThrow(TypeError);
    });

    test("answers remembered for the shapes of objects", () => {
        function has(object, key) {
            return object.hasOwnProperty(key);
        }
        function get(object, key) {
            return object[key];
        }
        const base = { inherited: 1 };
        const child = Object.create(base);
        child.own = 2;
        for (let i = 0; i < iterations; ++i) {
            expect(has(child, "own")).toBeTrue();
            expect(has(child, "inherited")).toBeFalse();
            expect(has(child, "missing")).toBeFalse();
            // Gets and `in` of the names that hasOwnProperty found missing look at the prototypes.
            expect(get(child, "inherited")).toBe(1);
            expect("inherited" in child).toBeTrue();
            expect(get(child, "missing")).toBeUndefined();
        }

        // Dictionaries change their properties without changing their shape.
        const dictionary = {};
        for (let i = 0; i < 100; ++i) dictionary["p" + i] = i;
        for (let i = 0; i < 100; ++i) delete dictionary["p" + i];
        for (let i = 0; i < iterations; ++i) {
            expect(has(dictionary, "late")).toBeFalse();
            dictionary.late = i;
            expect(has(dictionary, "late")).toBeTrue();
            delete dictionary.late;
        }

        // Functions make their prototype property when it is first asked for, and arrays have a length their shapes
        // do not hold.
        for (let i = 0; i < iterations; ++i) {
            const plain = Object.create(Function.prototype);
            expect(has(plain, "prototype")).toBeFalse();
            expect(has(function () {}, "prototype")).toBeTrue();
            expect(has({}, "length")).toBeFalse();
            expect(has([], "length")).toBeTrue();
        }
    });

    test("through Function.prototype.call", () => {
        const hasOwn = Object.prototype.hasOwnProperty;
        function has(object, key) {
            return hasOwn.call(object, key);
        }
        const object = { a: 1 };
        for (let i = 0; i < iterations; ++i) {
            expect(has(object, "a")).toBeTrue();
            expect(has(object, "z")).toBeFalse();
        }
        expect(has([], "length")).toBeTrue();
        expect(() => has(undefined, "a")).toThrow(TypeError);
    });

    test("proxies and replaced methods", () => {
        function has(object, key) {
            return object.hasOwnProperty(key);
        }
        for (let i = 0; i < iterations; ++i) expect(has({ a: 1 }, "a")).toBeTrue();
        const seen = [];
        const proxy = new Proxy(
            { a: 1 },
            {
                getOwnPropertyDescriptor(target, key) {
                    seen.push(key);
                    return Reflect.getOwnPropertyDescriptor(target, key);
                },
            }
        );
        expect(has(proxy, "a")).toBeTrue();
        expect(seen).toEqual(["a"]);
        const object = { a: 1, hasOwnProperty: () => "mine" };
        expect(has(object, "a")).toBe("mine");
    });
});

describe("the in operator", () => {
    test("own, inherited and missing properties", () => {
        function isIn(key, object) {
            return key in object;
        }
        const base = { inherited: 1 };
        const object = Object.create(base);
        object.own = 2;
        for (let i = 0; i < iterations; ++i) {
            expect(isIn("own", object)).toBeTrue();
            expect(isIn("inherited", object)).toBeTrue();
            expect(isIn("missing", object)).toBeFalse();
            expect(isIn("toString", object)).toBeTrue();
        }
        base.late = 3;
        expect(isIn("late", object)).toBeTrue();
        delete object.own;
        expect(isIn("own", object)).toBeFalse();
        const dictionary = {};
        for (let i = 0; i < 200; ++i) dictionary[`key${i}`] = i;
        for (let i = 0; i < 150; ++i) delete dictionary[`key${i}`];
        expect(isIn("key199", dictionary)).toBeTrue();
        expect(isIn("key0", dictionary)).toBeFalse();
        let getterCalls = 0;
        const withGetter = {
            get value() {
                ++getterCalls;
                return 1;
            },
        };
        expect(isIn("value", withGetter)).toBeTrue();
        expect(getterCalls).toBe(0);
        expect(isIn("0", ["x"])).toBeTrue();
        expect(isIn("1", ["x"])).toBeFalse();
        expect(isIn(1, [1, 2])).toBeTrue();
        expect(isIn("length", [])).toBeTrue();
        expect(() => isIn("a", "string")).toThrow(TypeError);
        expect(() => isIn("a", undefined)).toThrow(TypeError);
    });

    test("answers remembered for the shapes of objects", () => {
        function isIn(key, object) {
            return key in object;
        }
        const dictionary = {};
        for (let i = 0; i < 100; ++i) dictionary["p" + i] = i;
        for (let i = 0; i < 100; ++i) delete dictionary["p" + i];
        const base = { inherited: 1 };
        const child = Object.create(base);
        child.own = 2;
        for (let i = 0; i < iterations; ++i) {
            dictionary.late = i;
            expect(isIn("late", dictionary)).toBeTrue();
            delete dictionary.late;
            expect(isIn("late", dictionary)).toBeFalse();
            // Keys whose strings are no fly strings are looked up in the runtime.
            expect(isIn("o" + "wn".repeat(1), child)).toBeTrue();
            expect(child.hasOwnProperty("inherited")).toBeFalse();
            expect(isIn("inherited", child)).toBeTrue();
            expect(child.hasOwnProperty("missing")).toBeFalse();
            expect(isIn("missing", child)).toBeFalse();
            expect(isIn("prototype", function () {})).toBeTrue();
        }
    });

    test("proxies in the prototype chain", () => {
        function isIn(key, object) {
            return key in object;
        }
        for (let i = 0; i < iterations; ++i) expect(isIn("a", { a: 1 })).toBeTrue();
        const seen = [];
        const proxy = new Proxy(
            {},
            {
                has(target, key) {
                    seen.push(key);
                    return key === "trap";
                },
            }
        );
        expect(isIn("trap", proxy)).toBeTrue();
        expect(isIn("other", Object.create(proxy))).toBeFalse();
        expect(seen).toEqual(["trap", "other"]);
    });
});

describe("string builtins", () => {
    test("charCodeAt, charAt and fromCharCode", () => {
        function codeAt(string, index) {
            return string.charCodeAt(index);
        }
        function at(string, index) {
            return string.charAt(index);
        }
        function fromCode(code) {
            return String.fromCharCode(code);
        }
        const rope = "ab" + String(Math.random() > 2) + "é";
        for (let i = 0; i < iterations; ++i) {
            expect(codeAt("hello", i % 5)).toBe("hello".charCodeAt(i % 5));
            expect(at("hello", i % 5)).toBe("hello"[i % 5]);
            expect(fromCode(97 + (i % 26))).toBe("abcdefghijklmnopqrstuvwxyz"[i % 26]);
        }
        expect(codeAt(rope, rope.length - 1)).toBe(0xe9);
        expect(at(rope, rope.length - 1)).toBe("é");
        expect(codeAt("abc", 3)).toBeNaN();
        expect(codeAt("abc", -1)).toBeNaN();
        expect(codeAt("abc", 1.5)).toBe(98);
        expect(at("abc", 7)).toBe("");
        expect(fromCode(0x10061)).toBe("a");
        expect(fromCode(0xe9)).toBe("é");
        expect(fromCode("98")).toBe("b");
        expect(codeAt(new String("xyz"), 2)).toBe(122);
    });

    test("replaced builtins", () => {
        function codeAt(string, index) {
            return string.charCodeAt(index);
        }
        for (let i = 0; i < iterations; ++i) expect(codeAt("abc", 0)).toBe(97);
        const original = String.prototype.charCodeAt;
        String.prototype.charCodeAt = () => "replaced";
        try {
            expect(codeAt("abc", 0)).toBe("replaced");
        } finally {
            String.prototype.charCodeAt = original;
        }
        expect(codeAt("abc", 0)).toBe(97);
    });
});

describe("builtin constructors called as functions", () => {
    test("String", () => {
        function toString(value) {
            return String(value);
        }
        function empty() {
            return String();
        }
        const symbol = Symbol("s");
        const object = {
            toString() {
                return "converted";
            },
        };
        for (let i = 0; i < iterations; ++i) {
            expect(toString("a")).toBe("a");
            expect(toString(i)).toBe(`${i}`);
            expect(toString(1.5)).toBe("1.5");
            expect(toString(null)).toBe("null");
            expect(toString(undefined)).toBe("undefined");
            expect(toString(true)).toBe("true");
            expect(toString(10n)).toBe("10");
            expect(toString(symbol)).toBe("Symbol(s)");
            expect(empty()).toBe("");
        }
        expect(toString(object)).toBe("converted");
        expect(toString([1, 2])).toBe("1,2");
    });

    test("Array", () => {
        function make(value) {
            return Array(value);
        }
        function empty() {
            return Array();
        }
        for (let i = 0; i < iterations; ++i) {
            const holes = make(3);
            expect(holes).toHaveLength(3);
            expect(0 in holes).toBeFalse();
            expect(Object.getPrototypeOf(holes)).toBe(Array.prototype);
            expect(make("a")).toEqual(["a"]);
            expect(make(null)).toEqual([null]);
            expect(empty()).toEqual([]);
        }
        expect(() => make(-1)).toThrow(RangeError);
        expect(() => make(1.5)).toThrow(RangeError);
        expect(make(2 ** 31)).toHaveLength(2 ** 31);
    });

    test("Object", () => {
        function wrap(value) {
            return Object(value);
        }
        function empty() {
            return Object();
        }
        const object = { a: 1 };
        for (let i = 0; i < iterations; ++i) {
            expect(wrap(object)).toBe(object);
            const fresh = wrap(null);
            expect(Object.getPrototypeOf(fresh)).toBe(Object.prototype);
            expect(Object.keys(fresh)).toEqual([]);
            expect(wrap(undefined)).not.toBe(wrap(undefined));
            const number = wrap(i);
            expect(typeof number).toBe("object");
            expect(number.valueOf()).toBe(i);
            expect(wrap("s") instanceof String).toBeTrue();
            expect(typeof empty()).toBe("object");
        }
    });

    test("Boolean", () => {
        function toBoolean(value) {
            return Boolean(value);
        }
        function empty() {
            return Boolean();
        }
        for (let i = 0; i < iterations; ++i) {
            expect(toBoolean(0)).toBeFalse();
            expect(toBoolean(i + 1)).toBeTrue();
            expect(toBoolean("")).toBeFalse();
            expect(toBoolean("a")).toBeTrue();
            expect(toBoolean({})).toBeTrue();
            expect(toBoolean(null)).toBeFalse();
            expect(toBoolean(NaN)).toBeFalse();
            expect(empty()).toBeFalse();
        }
    });

    test("replaced constructors", () => {
        function toString(value) {
            return String(value);
        }
        for (let i = 0; i < iterations; ++i) expect(toString(i)).toBe(`${i}`);
        const original = globalThis.String;
        try {
            globalThis.String = value => `replaced ${value}`;
            expect(toString(1)).toBe("replaced 1");
        } finally {
            globalThis.String = original;
        }
        expect(toString(2)).toBe("2");
    });
});
