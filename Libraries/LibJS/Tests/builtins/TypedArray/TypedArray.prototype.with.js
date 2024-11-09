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

describe("errors", () => {
    test("index out of range", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);

            expect(() => {
                array.with(3, 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: 3");

            expect(() => {
                array.with(-4, 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: -1");
        });
    });

    test("invalid index", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);

            expect(() => {
                array.with(2 ** 53, 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: 9007199254740992");

            expect(() => {
                array.with(-(2 ** 53), 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: -9007199254740989");

            expect(() => {
                array.with(Infinity, 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: inf");

            expect(() => {
                array.with(-Infinity, 10);
            }).toThrowWithMessage(RangeError, "Invalid integer index: -inf");
        });
    });

    test("ArrayBuffer out of bounds", () => {
        TYPED_ARRAYS.forEach(T => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.with(0, 0);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        TYPED_ARRAYS.forEach(T => {
            expect(T.prototype.with).toHaveLength(2);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            expect(T.prototype.with).toHaveLength(2);
        });
    });

    test("basic functionality", () => {
        TYPED_ARRAYS.forEach(T => {
            const a = new T([1, 2, 3, 4, 5]);
            const values = [
                [0, 10, new T([10, 2, 3, 4, 5])],
                [-5, 10, new T([10, 2, 3, 4, 5])],
                [4, 10, new T([1, 2, 3, 4, 10])],
                [-1, 10, new T([1, 2, 3, 4, 10])],
            ];
            for (const [index, value, expected] of values) {
                const b = a.with(index, value);
                expect(a).not.toBe(b);
                expect(a).toEqual(new T([1, 2, 3, 4, 5]));
                expect(b).toEqual(expected);
            }
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const a = new T([1n, 2n, 3n, 4n, 5n]);
            const values = [
                [0, 10n, new T([10n, 2n, 3n, 4n, 5n])],
                [-5, 10n, new T([10n, 2n, 3n, 4n, 5n])],
                [4, 10n, new T([1n, 2n, 3n, 4n, 10n])],
                [-1, 10n, new T([1n, 2n, 3n, 4n, 10n])],
            ];
            for (const [index, value, expected] of values) {
                const b = a.with(index, value);
                expect(a).not.toBe(b);
                expect(a).toEqual(new T([1n, 2n, 3n, 4n, 5n]));
                expect(b).toEqual(expected);
            }
        });
    });
});
