const TYPED_ARRAYS = [
    { array: Uint8Array, expected: 3 },
    { array: Uint8ClampedArray, expected: 3 },
    { array: Uint16Array, expected: 6 },
    { array: Uint32Array, expected: 12 },
    { array: BigUint64Array, expected: 24 },
    { array: Int8Array, expected: 3 },
    { array: Int16Array, expected: 6 },
    { array: Int32Array, expected: 12 },
    { array: BigInt64Array, expected: 24 },
    { array: Float32Array, expected: 12 },
    { array: Float64Array, expected: 24 },
];

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        const isBigIntArray = T.array === BigInt64Array || T.array === BigUint64Array;
        let typedArray;

        if (!isBigIntArray) typedArray = new T.array([1, 2, 3]);
        else typedArray = new T.array([1n, 2n, 3n]);

        expect(Object.hasOwn(typedArray, "byteLength")).toBeFalse();
        expect(typedArray.byteLength).toBe(T.expected);
    });
});

test("resizable ArrayBuffer", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.array.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.array.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new T.array(arrayBuffer, T.array.BYTES_PER_ELEMENT, 1);
        expect(typedArray.byteLength).toBe(T.array.BYTES_PER_ELEMENT);

        arrayBuffer.resize(T.array.BYTES_PER_ELEMENT);
        expect(typedArray.byteLength).toBe(0);
    });
});
