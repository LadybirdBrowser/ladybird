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

    test("set preserves bit encodings between same-width signed and unsigned integer arrays", () => {
        const pairs = [
            [Uint8Array, Int8Array, [0, 127, 128, 255], [0, 127, -128, -1]],
            [Uint16Array, Int16Array, [0, 32767, 32768, 65535], [0, 32767, -32768, -1]],
            [Uint32Array, Int32Array, [0, 2147483647, 2147483648, 4294967295], [0, 2147483647, -2147483648, -1]],
            [
                BigUint64Array,
                BigInt64Array,
                [0n, 9223372036854775807n, 9223372036854775808n, 18446744073709551615n],
                [0n, 9223372036854775807n, -9223372036854775808n, -1n],
            ],
        ];

        for (const [UnsignedArray, SignedArray, unsignedValues, signedValues] of pairs) {
            const signedTarget = new SignedArray(unsignedValues.length);
            signedTarget.set(new UnsignedArray(unsignedValues));
            expect(Array.from(signedTarget)).toEqual(signedValues);

            const unsignedTarget = new UnsignedArray(signedValues.length);
            unsignedTarget.set(new SignedArray(signedValues));
            expect(Array.from(unsignedTarget)).toEqual(unsignedValues);
        }
    });

    test("set snapshots overlapping same-width signed and unsigned integer arrays", () => {
        const buffer = new ArrayBuffer(4);
        const source = new Uint8Array(buffer);
        source.set([1, 2, 3, 4]);

        new Int8Array(buffer, 1).set(source.subarray(0, 3));

        expect(Array.from(source)).toEqual([1, 1, 2, 3]);
    });

    function bufferWithElements(ArrayType, values, BufferType = ArrayBuffer) {
        const buffer = new BufferType(values.length * ArrayType.BYTES_PER_ELEMENT);
        new ArrayType(buffer).set(values);
        return buffer;
    }

    test("set snapshots a source with smaller elements on the same buffer", () => {
        let buffer = bufferWithElements(Uint8Array, [1, 2, 3, 4, 5, 6, 7, 8]);
        let target = new Uint16Array(buffer, 0, 2);
        target.set(new Uint8Array(buffer, 4, 2));
        expect(Array.from(target)).toEqual([5, 6]);

        buffer = bufferWithElements(Uint8Array, [1, 2, 3, 4, 5, 6, 7, 8]);
        target = new Uint16Array(buffer);
        target.set(new Uint8Array(buffer, 2, 4));
        expect(Array.from(target)).toEqual([3, 4, 5, 6]);

        buffer = bufferWithElements(Uint8Array, [1, 2, 3, 4, 5, 6, 7, 8]);
        new Uint16Array(buffer).set(new Uint8Array(buffer, 1, 3), 1);
        expect(Array.from(new Uint8Array(buffer))).toEqual([1, 2, 2, 0, 3, 0, 4, 0]);
    });

    test("set snapshots a source with larger elements on the same buffer", () => {
        let buffer = bufferWithElements(Uint16Array, [1, 2, 300, 7]);
        new Uint8Array(buffer, 0, 2).set(new Uint16Array(buffer, 4, 2));
        expect(Array.from(new Uint8Array(buffer))).toEqual([44, 7, 2, 0, 44, 1, 7, 0]);

        buffer = bufferWithElements(Uint16Array, [10, 20, 30, 40]);
        new Uint8Array(buffer).set(new Uint16Array(buffer), 2);
        expect(Array.from(new Uint8Array(buffer))).toEqual([10, 0, 10, 20, 30, 40, 40, 0]);
    });

    test("set snapshots a floating point source on the same buffer", () => {
        let buffer = bufferWithElements(Float64Array, [1.5, -2.25, 3.75]);
        let target = new Float32Array(buffer, 0, 2);
        target.set(new Float64Array(buffer, 8, 2));
        expect(Array.from(target)).toEqual([-2.25, 3.75]);

        buffer = bufferWithElements(Float32Array, [1.5, -2.25, 3.75, 100]);
        target = new Float64Array(buffer);
        target.set(new Float32Array(buffer, 8, 2));
        expect(Array.from(target)).toEqual([3.75, 100]);

        buffer = bufferWithElements(Int8Array, [-7, 9, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        target = new Float32Array(buffer);
        target.set(new Int8Array(buffer, 0, 3));
        expect(Array.from(target)).toEqual([-7, 9, 11]);
    });

    test("set snapshots a BigInt source on the same buffer", () => {
        const buffer = bufferWithElements(BigInt64Array, [-1n, -2n, 3n, 4n]);
        const target = new BigUint64Array(buffer);
        target.set(new BigInt64Array(buffer, 0, 3), 1);
        expect(Array.from(target)).toEqual([2n ** 64n - 1n, 2n ** 64n - 1n, 2n ** 64n - 2n, 3n]);
    });

    test("set snapshots a source with another element type on the same shared buffer", () => {
        const buffer = bufferWithElements(Uint8Array, [1, 2, 3, 4, 5, 6, 7, 8], SharedArrayBuffer);
        const target = new Uint16Array(buffer, 0, 2);
        target.set(new Uint8Array(buffer, 4, 2));
        expect(Array.from(target)).toEqual([5, 6]);
    });

    test("set snapshots a length-tracking source with another element type on the same buffer", () => {
        const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
        new Uint8Array(buffer).set([1, 2, 3, 4, 5, 6, 7, 8]);
        const target = new Uint16Array(buffer, 0, 2);
        target.set(new Uint8Array(buffer, 6));
        expect(Array.from(target)).toEqual([7, 8]);
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
            // set_typed_array_from_typed_array
            typedArray.set(typedArray, 2 ** 64);
        }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target offset");

        expect(() => {
            // set_typed_array_from_array_like
            typedArray.set([], 2 ** 128);
        }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target offset");

        expect(() => {
            // set_typed_array_from_array_like
            typedArray.set([], 2 ** 64);
        }).toThrowWithMessage(RangeError, "Overflow or out of bounds in target offset");
    });
});
