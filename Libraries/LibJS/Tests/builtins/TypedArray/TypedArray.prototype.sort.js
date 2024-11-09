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
    test("ArrayBuffer out of bounds", () => {
        TYPED_ARRAYS.forEach(T => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.sort();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    });
});

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.sort).toHaveLength(1);

        const typedArray = new T(3);
        typedArray[0] = 3;
        typedArray[1] = 1;
        typedArray[2] = 2;

        expect(typedArray.sort()).toBe(typedArray);
        expect(typedArray[0]).toBe(1);
        expect(typedArray[1]).toBe(2);
        expect(typedArray[2]).toBe(3);

        expect(typedArray.sort(() => 1)).toBe(typedArray);
        expect(typedArray[0]).toBe(3);
        expect(typedArray[1]).toBe(2);
        expect(typedArray[2]).toBe(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.sort).toHaveLength(1);

        const typedArray = new T(3);
        typedArray[0] = 3n;
        typedArray[1] = 1n;
        typedArray[2] = 2n;

        expect(typedArray.sort()).toBe(typedArray);
        expect(typedArray[0]).toBe(1n);
        expect(typedArray[1]).toBe(2n);
        expect(typedArray[2]).toBe(3n);

        expect(typedArray.sort(() => 1)).toBe(typedArray);
        expect(typedArray[0]).toBe(3n);
        expect(typedArray[1]).toBe(2n);
        expect(typedArray[2]).toBe(1n);
    });
});

test("detached buffer", () => {
    TYPED_ARRAYS.forEach(T => {
        const typedArray = new T(3);
        typedArray[0] = 3;
        typedArray[1] = 1;
        typedArray[2] = 2;

        typedArray.sort((a, b) => {
            detachArrayBuffer(typedArray.buffer);
            return a - b;
        });

        expect(typedArray[0]).toBeUndefined();
        expect(typedArray[1]).toBeUndefined();
        expect(typedArray[2]).toBeUndefined();
    });
});
