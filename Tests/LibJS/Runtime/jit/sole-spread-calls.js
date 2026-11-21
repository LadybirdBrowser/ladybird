function compileSpreadCall(f, ...warmArguments) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

describe("calls with one spread argument", () => {
    test("packed arrays, missing parameters, rest parameters, and native callees", () => {
        function collect(a, b, c) {
            return [a, b, c, arguments.length];
        }
        const call = compileSpreadCall((callee, iterable) => callee(...iterable), collect, [1, 2]);
        const rest = compileSpreadCall((callee, ...values) => callee(...values), collect, 1, 2);

        expect(call(collect, [])).toEqual([undefined, undefined, undefined, 0]);
        expect(call(collect, [1, 2])).toEqual([1, 2, undefined, 2]);
        expect(call(collect, [1, 2, 3, 4])).toEqual([1, 2, 3, 4]);
        expect(rest(collect, 1, 2)).toEqual([1, 2, undefined, 2]);
        expect(call(Math.max, [1, 7, 3])).toBe(7);
        expect(call(collect.bind(null, 9), [1, 2])).toEqual([9, 1, 2, 3]);
        expect(call(new Proxy(collect, {}), [1, 2])).toEqual([1, 2, undefined, 2]);
    });

    test("iteration keeps the callee and receiver evaluated before it", () => {
        const original = {
            label: "original",
            method(...values) {
                return [this.label, values];
            },
        };
        const call = compileSpreadCall((object, iterable) => object.method(...iterable), original, [1]);
        let lookups = 0;
        const iterable = [42];
        Object.defineProperty(iterable, Symbol.iterator, {
            get() {
                ++lookups;
                original.method = () => "replacement";
                iterable.push(43);
                gc();
                return Array.prototype.values;
            },
        });

        expect(call(original, iterable)).toEqual(["original", [42, 43]]);
        expect(lookups).toBe(1);
    });

    test("assignments in the iterable expression preserve the original call reference", () => {
        const original = value => value + 1;
        const replacement = () => "replacement";
        const call = compileSpreadCall(
            (callee, newer, iterable) => callee(...((callee = newer), iterable)),
            original,
            replacement,
            [1]
        );
        expect(call(original, replacement, [41])).toBe(42);

        const object = {
            value: 40,
            method(value) {
                return this.value + value;
            },
        };
        const other = { value: 100, method: replacement };
        const callMethod = compileSpreadCall(
            (base, newer, iterable) => base.method(...((base = newer), [...iterable])),
            object,
            other,
            [1]
        );
        expect(callMethod(object, other, [2])).toBe(42);
    });

    test("generic iteration finishes before testing callability", () => {
        const call = compileSpreadCall(
            (callee, iterable) => callee(...iterable),
            () => 1,
            []
        );
        const log = [];
        const iterable = {
            get [Symbol.iterator]() {
                log.push("iterator");
                return function () {
                    let done = false;
                    return {
                        next() {
                            log.push("next");
                            const result = { value: 1, done };
                            done = true;
                            return result;
                        },
                    };
                };
            },
        };

        expect(() => call(undefined, iterable)).toThrow(TypeError);
        expect(log).toEqual(["iterator", "next", "next"]);
    });

    test("iterator exceptions preserve the previous result in the catch handler", () => {
        function call(callee, iterable) {
            let result = 17;
            try {
                result = callee(...iterable);
            } catch (error) {
                return [result, error];
            }
            return result;
        }
        compileSpreadCall(call, () => 1, []);
        const failure = {};
        const iterable = {
            [Symbol.iterator]() {
                return {
                    next() {
                        throw failure;
                    },
                };
            },
        };
        expect(call(undefined, iterable)).toEqual([17, failure]);
    });

    test("holes and prototype getters follow ordinary array iteration", () => {
        const call = compileSpreadCall(
            (callee, iterable) => callee(...iterable),
            (...values) => values,
            [1]
        );
        const collect = (...values) => values;
        const array = [, 2];
        const prototype = Object.create(Array.prototype);
        Object.defineProperty(prototype, "0", {
            get() {
                array.push(3);
                return 1;
            },
        });
        Object.setPrototypeOf(array, prototype);

        expect(call(collect, array)).toEqual([1, 2, 3]);
        expect(call(collect, [, 2])).toEqual([undefined, 2]);
        expect(call(collect, "a\u{1f600}")).toEqual(["a", "\u{1f600}"]);
        expect(call(collect, new Set([1, 2]))).toEqual([1, 2]);
    });

    test("replacing array iterator next remains observable after compilation", () => {
        const call = compileSpreadCall(
            (callee, iterable) => callee(...iterable),
            (a, b) => a + b,
            [1, 2]
        );
        const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
        const originalNext = iteratorPrototype.next;
        let steps = 0;
        iteratorPrototype.next = function () {
            ++steps;
            return originalNext.call(this);
        };
        try {
            expect(call((a, b) => a + b, [1, 2])).toBe(3);
            expect(steps).toBe(3);
        } finally {
            iteratorPrototype.next = originalNext;
        }
    });

    test("values from a generic iterator stay alive across collections", () => {
        const call = compileSpreadCall(
            (callee, iterable) => callee(...iterable),
            value => value,
            [1]
        );
        const iterable = {
            *[Symbol.iterator]() {
                yield { value: 1 };
                gc();
                yield { value: 2 };
                gc();
            },
        };
        expect(call((a, b) => [a.value, b.value], iterable)).toEqual([1, 2]);
    });

    test("optional calls skip the spread and other call kinds retain their semantics", () => {
        let evaluated = false;
        const absent = undefined;
        expect(absent?.(...((evaluated = true), []))).toBeUndefined();
        expect(evaluated).toBeFalse();

        function evaluate() {
            let local = 1;
            eval(...["local = 2"]);
            return local;
        }
        class Base {
            constructor(value) {
                this.value = value;
            }
        }
        class Derived extends Base {
            constructor(...args) {
                super(...args);
            }
        }
        expect(evaluate()).toBe(2);
        expect(new Derived(...[42]).value).toBe(42);
    });
});
