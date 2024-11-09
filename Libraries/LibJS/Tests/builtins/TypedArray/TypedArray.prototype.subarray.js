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

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.subarray).toHaveLength(2);

        const typedArray = new T(3);
        typedArray[0] = 1;
        typedArray[1] = 2;
        typedArray[2] = 3;

        const subarray = typedArray.subarray(1, 2);
        expect(subarray).toHaveLength(1);
        expect(subarray[0]).toBe(2);
        subarray[0] = 4;
        expect(typedArray[1]).toBe(4);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.subarray).toHaveLength(2);

        const typedArray = new T(3);
        typedArray[0] = 1n;
        typedArray[1] = 2n;
        typedArray[2] = 3n;

        const subarray = typedArray.subarray(1, 2);
        expect(subarray).toHaveLength(1);
        expect(subarray[0]).toBe(2n);
        subarray[0] = 4n;
        expect(typedArray[1]).toBe(4n);
    });
});

test("resizable ArrayBuffer", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
        expect(typedArray.subarray(0, 1).byteLength).toBe(T.BYTES_PER_ELEMENT);

        arrayBuffer.resize(T.BYTES_PER_ELEMENT);
        expect(typedArray.subarray(0, 1).byteLength).toBe(0);
    });
});

test("resizable ArrayBuffer resized during `start` parameter access", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let badAccessor = {
            valueOf: () => {
                arrayBuffer.resize(T.BYTES_PER_ELEMENT * 4);
                return 0;
            },
        };

        let typedArray = new T(arrayBuffer);
        expect(typedArray.subarray(badAccessor, typedArray.length).length).toBe(2);
    });
});
