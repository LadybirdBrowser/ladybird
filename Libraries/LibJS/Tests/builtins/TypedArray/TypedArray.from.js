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
        const newTypedArray = T.from([1, 2, 3]);
        expect(newTypedArray[0]).toBe(1);
        expect(newTypedArray[1]).toBe(2);
        expect(newTypedArray[2]).toBe(3);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        const newTypedArray = T.from([1n, 2n, 3n]);
        expect(newTypedArray[0]).toBe(1n);
        expect(newTypedArray[1]).toBe(2n);
        expect(newTypedArray[2]).toBe(3n);
    });
});

test("is inherited from TypedArray base class", () => {
    TYPED_ARRAYS.forEach(T1 => {
        TYPED_ARRAYS.forEach(T2 => {
            expect(T1.from).toBe(T2.from);
        });
    });
});
