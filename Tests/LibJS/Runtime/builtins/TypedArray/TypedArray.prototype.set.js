const TYPED_ARRAYS = [
    { array: Uint8Array, maxUnsignedInteger: 2 ** 8 - 1 },
    { array: Uint8ClampedArray, maxUnsignedInteger: 2 ** 8 - 1 },
    { array: Uint16Array, maxUnsignedInteger: 2 ** 16 - 1 },
    { array: Uint32Array, maxUnsignedInteger: 2 ** 32 - 1 },
    { array: Int8Array, maxUnsignedInteger: 2 ** 7 - 1 },
    { array: Int16Array, maxUnsignedInteger: 2 ** 15 - 1 },
    { array: Int32Array, maxUnsignedInteger: 2 ** 31 - 1 },
    { array: Float16Array, maxUnsignedInteger: 2 ** 11 - 1 },
    { array: Float32Array, maxUnsignedInteger: 2 ** 24 - 1 },
    { array: Float64Array, maxUnsignedInteger: Number.MAX_SAFE_INTEGER },
];

const BIGINT_TYPED_ARRAYS = [
    { array: BigUint64Array, maxUnsignedInteger: 2n ** 64n - 1n },
    { array: BigInt64Array, maxUnsignedInteger: 2n ** 63n - 1n },
];

describe("errors", () => {
    function argumentErrorTests(T) {
        test(`requires at least one argument (${T.name})`, () => {
            expect(() => {
                new T().set();
            }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
        });

        test(`source array in bounds (${T.name})`, () => {
            expect(() => {
                new T().set([0]);
            }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target length");
        });

        test(`ArrayBuffer out of bounds  (${T.name})`, () => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.set([0]);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );

            expect(() => {
                typedArray.set(new T());
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    }

    TYPED_ARRAYS.forEach(({ array: T }) => argumentErrorTests(T));
    BIGINT_TYPED_ARRAYS.forEach(({ array: T }) => argumentErrorTests(T));
});

// FIXME: Write out a full test suite for this function. This currently only performs a single regression test.
describe("normal behavior", () => {
    // Previously, we didn't apply source's byte offset on the code path for setting a typed array
    // from another typed array of the same type. This means the result array would previously contain
    // [maxUnsignedInteger - 3(n), maxUnsignedInteger - 2(n)] instead of [maxUnsignedInteger - 1(n), maxUnsignedInteger]
    test("two typed arrays of the same type code path applies source's byte offset", () => {
        TYPED_ARRAYS.forEach(({ array, maxUnsignedInteger }) => {
            const firstTypedArray = new array([
                maxUnsignedInteger - 3,
                maxUnsignedInteger - 2,
                maxUnsignedInteger - 1,
                maxUnsignedInteger,
            ]);
            const secondTypedArray = new array(2);
            secondTypedArray.set(firstTypedArray.subarray(2, 4), 0);
            expect(secondTypedArray[0]).toBe(maxUnsignedInteger - 1);
            expect(secondTypedArray[1]).toBe(maxUnsignedInteger);
        });

        BIGINT_TYPED_ARRAYS.forEach(({ array, maxUnsignedInteger }) => {
            const firstTypedArray = new array([
                maxUnsignedInteger - 3n,
                maxUnsignedInteger - 2n,
                maxUnsignedInteger - 1n,
                maxUnsignedInteger,
            ]);
            const secondTypedArray = new array(2);
            secondTypedArray.set(firstTypedArray.subarray(2, 4), 0);
            expect(secondTypedArray[0]).toBe(maxUnsignedInteger - 1n);
            expect(secondTypedArray[1]).toBe(maxUnsignedInteger);
        });
    });

    test("set works when source is TypedArray", () => {
        function argumentTests({ array, maxUnsignedInteger }) {
            const firstTypedArray = new array(1);
            const secondTypedArray = new array([maxUnsignedInteger]);
            firstTypedArray.set(secondTypedArray, 0);
            expect(firstTypedArray[0]).toBe(maxUnsignedInteger);
        }

        TYPED_ARRAYS.forEach(T => argumentTests(T));
        BIGINT_TYPED_ARRAYS.forEach(T => argumentTests(T));
    });

    test("set works when source is Array", () => {
        function argumentTests({ array, maxUnsignedInteger }) {
            const firstTypedArray = new array(1);
            firstTypedArray.set([maxUnsignedInteger], 0);
            expect(firstTypedArray[0]).toBe(maxUnsignedInteger);
        }

        TYPED_ARRAYS.forEach(T => argumentTests(T));
        BIGINT_TYPED_ARRAYS.forEach(T => argumentTests(T));
    });
});

test("length is 1", () => {
    TYPED_ARRAYS.forEach(({ array: T }) => {
        expect(T.prototype.set).toHaveLength(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(({ array: T }) => {
        expect(T.prototype.set).toHaveLength(1);
    });
});

test("detached buffer", () => {
    TYPED_ARRAYS.forEach(({ array: T }) => {
        let typedArray = new T(2);
        typedArray[0] = 1;
        typedArray[1] = 2;

        let object = { length: 2 };

        Object.defineProperty(object, 0, {
            get: () => {
                detachArrayBuffer(typedArray.buffer);
            },
        });

        expect(() => {
            typedArray.set(object);
        }).not.toThrow();

        expect(typedArray.length).toBe(0);
    });
});

test("very large targetOffset", () => {
    TYPED_ARRAYS.forEach(({ array: T }) => {
        let typedArray = new T();

        expect(() => {
            // set_typed_array_from_typed_array
            typedArray.set(typedArray, 2 ** 128);
        }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target offset");

        expect(() => {
            // set_typed_array_from_array_like
            typedArray.set([], 2 ** 128);
        }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target offset");
    });
});
