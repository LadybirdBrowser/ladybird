const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    BigUint64Array,
    Int8Array,
    Int16Array,
    Int32Array,
    BigInt64Array,
    Float32Array,
    Float64Array,
];

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        const isBigIntArray = T === BigInt64Array || T === BigUint64Array;
        let typedArray;

        if (!isBigIntArray) typedArray = new T([1, 2, 3]);
        else typedArray = new T([1n, 2n, 3n]);

        expect(Object.hasOwn(typedArray, "byteOffset")).toBeFalse();
        expect(typedArray.byteOffset).toBe(0);
        expect(typedArray.length).toBe(3);

        const buffer = typedArray.buffer;

        const arrayFromOffset = new T(buffer, T.BYTES_PER_ELEMENT);
        expect(arrayFromOffset.byteOffset).toBe(T.BYTES_PER_ELEMENT);
        expect(arrayFromOffset.length).toBe(2);
        expect(arrayFromOffset[0]).toBe(!isBigIntArray ? 2 : 2n);
        expect(arrayFromOffset[1]).toBe(!isBigIntArray ? 3 : 3n);
    });
});

test("resizable ArrayBuffer", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
        expect(typedArray.byteOffset).toBe(T.BYTES_PER_ELEMENT);

        arrayBuffer.resize(T.BYTES_PER_ELEMENT);
        expect(typedArray.byteOffset).toBe(0);
    });
});
