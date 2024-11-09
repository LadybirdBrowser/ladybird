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
        const typedArray = new T(42);
        expect(Object.hasOwnProperty(typedArray, "length")).toBeFalse();
        expect(typedArray.length).toBe(42);
    });
});

test("resizable ArrayBuffer", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
        expect(typedArray.length).toBe(1);

        arrayBuffer.resize(T.BYTES_PER_ELEMENT);
        expect(typedArray.length).toBe(0);
    });
});
