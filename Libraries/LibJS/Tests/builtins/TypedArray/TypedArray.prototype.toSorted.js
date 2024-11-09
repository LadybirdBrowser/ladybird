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
    test("null or undefined this value", () => {
        TYPED_ARRAYS.forEach(T => {
            expect(() => {
                T.prototype.toSorted.call();
            }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

            expect(() => {
                T.prototype.toSorted.call(undefined);
            }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

            expect(() => {
                T.prototype.toSorted.call(null);
            }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {});
    });

    test("invalid compare function", () => {
        TYPED_ARRAYS.forEach(T => {
            expect(() => {
                new T([]).toSorted("foo");
            }).toThrowWithMessage(TypeError, "foo is not a function");
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            expect(() => {
                new T([]).toSorted("foo");
            }).toThrowWithMessage(TypeError, "foo is not a function");
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
                typedArray.toSorted();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    });
});

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.toSorted).toHaveLength(1);

        const typedArray = new T([3, 1, 2]);
        let sortedtypedArray = typedArray.toSorted();
        expect(sortedtypedArray).not.toBe(typedArray);
        expect(sortedtypedArray[0]).toBe(1);
        expect(sortedtypedArray[1]).toBe(2);
        expect(sortedtypedArray[2]).toBe(3);

        sortedtypedArray = typedArray.toSorted(() => 0);
        expect(sortedtypedArray).not.toBe(typedArray);
        expect(sortedtypedArray[0]).toBe(3);
        expect(sortedtypedArray[1]).toBe(1);
        expect(sortedtypedArray[2]).toBe(2);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.toSorted).toHaveLength(1);

        const typedArray = new T([3n, 1n, 2n]);

        let sortedtypedArray = typedArray.toSorted();
        expect(sortedtypedArray).not.toBe(typedArray);
        expect(sortedtypedArray[0]).toBe(1n);
        expect(sortedtypedArray[1]).toBe(2n);
        expect(sortedtypedArray[2]).toBe(3n);

        sortedtypedArray = typedArray.toSorted(() => 0);
        expect(sortedtypedArray).not.toBe(typedArray);
        expect(sortedtypedArray[0]).toBe(3n);
        expect(sortedtypedArray[1]).toBe(1n);
        expect(sortedtypedArray[2]).toBe(2n);
    });
});

test("detached buffer", () => {
    TYPED_ARRAYS.forEach(T => {
        const typedArray = new T(3);
        typedArray[0] = 3;
        typedArray[1] = 1;
        typedArray[2] = 2;

        const sortedTypedArray = typedArray.toSorted((a, b) => {
            detachArrayBuffer(typedArray.buffer);
            return a - b;
        });

        expect(typedArray[0]).toBeUndefined();
        expect(typedArray[1]).toBeUndefined();
        expect(typedArray[2]).toBeUndefined();

        expect(sortedTypedArray[0]).toBe(1);
        expect(sortedTypedArray[1]).toBe(2);
        expect(sortedTypedArray[2]).toBe(3);
    });
});
