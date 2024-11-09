const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

const ALL_TYPED_ARRAYS = TYPED_ARRAYS.concat(BIGINT_TYPED_ARRAYS);

describe("correct behavior", () => {
    test("length is 0", () => {
        ALL_TYPED_ARRAYS.forEach(T => {
            expect(T.prototype[Symbol.iterator]).toHaveLength(0);
        });
    });

    test("same value as %TypedArray%.prototype.values", () => {
        ALL_TYPED_ARRAYS.forEach(T => {
            expect(T.prototype[Symbol.iterator]).toBe(T.prototype.values);
        });
    });

    test("basic functionality", () => {
        TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1, 2, 3]);
            const iterator = typedArray[Symbol.iterator]();
            expect(iterator.next()).toEqual({ value: 1, done: false });
            expect(iterator.next()).toEqual({ value: 2, done: false });
            expect(iterator.next()).toEqual({ value: 3, done: false });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1n, 2n, 3n]);
            const iterator = typedArray[Symbol.iterator]();
            expect(iterator.next()).toEqual({ value: 1n, done: false });
            expect(iterator.next()).toEqual({ value: 2n, done: false });
            expect(iterator.next()).toEqual({ value: 3n, done: false });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
        });
    });

    test("can be iterated with for-of", () => {
        TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1, 2, 3]);
            const result = [];

            for (const value of typedArray) result.push(value);

            expect(result).toHaveLength(3);
            expect(result[0]).toBe(1);
            expect(result[1]).toBe(2);
            expect(result[2]).toBe(3);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1n, 2n, 3n]);
            const result = [];

            for (const value of typedArray) result.push(value);

            expect(result).toHaveLength(3);
            expect(result[0]).toBe(1n);
            expect(result[1]).toBe(2n);
            expect(result[2]).toBe(3n);
        });
    });
});

describe("errors", () => {
    test("this value must be a TypedArray object", () => {
        ALL_TYPED_ARRAYS.forEach(T => {
            expect(() => {
                T.prototype[Symbol.iterator].call({});
            }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");
        });
    });
});
