const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float16Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

describe("errors", () => {
    test("ArrayBuffer out of bounds", () => {
        TYPED_ARRAYS.forEach(T => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.copyWithin(0, 0);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    });
});

test("length is 2", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.copyWithin).toHaveLength(2);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.copyWithin).toHaveLength(2);
    });
});

describe("normal behavior", () => {
    test("Noop", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2]);
            expect(array.copyWithin(0, 0)).toEqual(array);
            expect(array).toEqual(new T([1, 2]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([1n, 2n]);
            expect(array.copyWithin(0, 0)).toEqual(array);
            expect(array).toEqual(new T([1n, 2n]));
        });
    });

    test("basic behavior", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);
            expect(array.copyWithin(1, 2)).toEqual(array);
            expect(array).toEqual(new T([1, 3, 3]));

            expect(array.copyWithin(2, 0)).toEqual(array);
            expect(array).toEqual(new T([1, 3, 1]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([1n, 2n, 3n]);
            expect(array.copyWithin(1, 2)).toEqual(array);
            expect(array).toEqual(new T([1n, 3n, 3n]));

            expect(array.copyWithin(2, 0)).toEqual(array);
            expect(array).toEqual(new T([1n, 3n, 1n]));
        });
    });

    test("start > target", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);
            expect(array.copyWithin(0, 1)).toEqual(array);
            expect(array).toEqual(new T([2, 3, 3]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([1n, 2n, 3n]);
            expect(array.copyWithin(0, 1)).toEqual(array);
            expect(array).toEqual(new T([2n, 3n, 3n]));
        });
    });

    test("overwriting behavior", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);
            expect(array.copyWithin(1, 0)).toEqual(array);
            expect(array).toEqual(new T([1, 1, 2]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([1n, 2n, 3n]);
            expect(array.copyWithin(1, 0)).toEqual(array);
            expect(array).toEqual(new T([1n, 1n, 2n]));
        });
    });

    test("specify end", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([1, 2, 3]);
            expect(array.copyWithin(2, 0, 1)).toEqual(array);
            expect(array).toEqual(new T([1, 2, 1]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([1n, 2n, 3n]);
            expect(array.copyWithin(2, 0, 1)).toEqual(array);
            expect(array).toEqual(new T([1n, 2n, 1n]));
        });
    });

    test("overwriting in middle with end", () => {
        TYPED_ARRAYS.forEach(T => {
            const array = new T([0, 54, 999, 1000, 0, 0]);
            expect(array.copyWithin(3, 2, 4)).toEqual(array);
            expect(array).toEqual(new T([0, 54, 999, 999, 1000, 0]));
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const array = new T([0n, 54n, 999n, 1000n, 0n, 0n]);
            expect(array.copyWithin(3, 2, 4)).toEqual(array);
            expect(array).toEqual(new T([0n, 54n, 999n, 999n, 1000n, 0n]));
        });
    });

    test("resizing during copyWithin", () => {
        TYPED_ARRAYS.forEach(T => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4, {
                maxByteLength: T.BYTES_PER_ELEMENT * 8,
            });

            let typedArray = new T(arrayBuffer);
            typedArray[0] = 1;
            typedArray[1] = 2;
            typedArray[2] = 3;
            typedArray[3] = 4;

            const resize = () => {
                arrayBuffer.resize(3 * T.BYTES_PER_ELEMENT);
                return 2;
            };

            typedArray.copyWithin({ valueOf: resize }, 1);
            expect(typedArray).toEqual(new T([1, 2, 2]));
        });
    });
});
