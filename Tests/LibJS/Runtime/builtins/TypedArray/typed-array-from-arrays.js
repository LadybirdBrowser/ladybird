// Typed array constructors and %TypedArray%.from take the values of arrays like the iterator protocol does, whether
// iterating the array can be observed or not.

const create = {
    constructor: (T, source) => new T(source),
    from: (T, source) => T.from(source),
};

describe("typed arrays from arrays", () => {
    for (const [name, make] of Object.entries(create)) {
        test(`${name}: packed arrays`, () => {
            const source = [];
            for (let i = 0; i < 1000; ++i) source.push((i * 31) & 255);
            const array = make(Uint8Array, source);
            expect(array.length).toBe(1000);
            for (let i = 0; i < 1000; ++i) expect(array[i]).toBe((i * 31) & 255);
            expect(Array.from(make(Int8Array, [127, 128, -129, 1.5, "3", true]))).toEqual([127, -128, 127, 1, 3, 1]);
            expect(Array.from(make(Float64Array, [0.5, -0, null, undefined]))).toEqual([0.5, -0, 0, NaN]);
            expect(Array.from(make(BigInt64Array, [1n, -2n]))).toEqual([1n, -2n]);
        });

        test(`${name}: holes and accessors read through the prototype chain`, () => {
            expect(Array.from(make(Int32Array, [1, , 3]))).toEqual([1, 0, 3]);
            Array.prototype[1] = 42;
            try {
                expect(Array.from(make(Int32Array, [1, , 3]))).toEqual([1, 42, 3]);
            } finally {
                delete Array.prototype[1];
            }
            const withGetter = [1, 2, 3];
            Object.defineProperty(withGetter, 1, { get: () => 7 });
            expect(Array.from(make(Int32Array, withGetter))).toEqual([1, 7, 3]);
        });

        test(`${name}: elements convert in order`, () => {
            const order = [];
            const element = value => ({
                valueOf() {
                    order.push(value);
                    return value;
                },
            });
            expect(Array.from(make(Uint16Array, [element(1), element(2), element(3)]))).toEqual([1, 2, 3]);
            expect(order).toEqual([1, 2, 3]);
            const throwing = [
                1,
                {
                    valueOf() {
                        throw new Error("element");
                    },
                },
            ];
            expect(() => make(Uint8Array, throwing)).toThrowWithMessage(Error, "element");
            expect(() => make(BigInt64Array, [1])).toThrow(TypeError);
        });

        test(`${name}: replaced iterators are used`, () => {
            const source = [1, 2, 3];
            source[Symbol.iterator] = function* () {
                yield 9;
            };
            expect(Array.from(make(Uint8Array, source))).toEqual([9]);

            const values = Array.prototype[Symbol.iterator];
            Array.prototype[Symbol.iterator] = function* () {
                yield* values.call(this);
                yield 4;
            };
            try {
                expect(Array.from(make(Uint8Array, [1, 2, 3]))).toEqual([1, 2, 3, 4]);
            } finally {
                Array.prototype[Symbol.iterator] = values;
            }

            const iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
            const next = iteratorPrototype.next;
            let calls = 0;
            iteratorPrototype.next = function () {
                ++calls;
                return next.call(this);
            };
            try {
                const array = make(Uint8Array, [1, 2, 3]);
                expect(calls).toBe(4);
                expect(Array.from(array)).toEqual([1, 2, 3]);
            } finally {
                iteratorPrototype.next = next;
            }
        });
    }

    test("from: mapping functions see the values and may detach the buffer", () => {
        let target;
        class Captured extends Uint8Array {
            constructor(length) {
                super(length);
                target = this;
            }
        }
        const mapped = Captured.from([1, 2, 3], (value, index) => {
            if (index === 1) target.buffer.transfer();
            return value * 10;
        });
        expect(mapped).toBe(target);
        expect(mapped.length).toBe(0);
        expect(Array.from(Uint8Array.from([1, 2, 3], value => value * 2))).toEqual([2, 4, 6]);
    });
});
