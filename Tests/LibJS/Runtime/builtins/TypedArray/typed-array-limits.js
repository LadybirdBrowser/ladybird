test("some oversized typed arrays", () => {
    expect(() => new Uint8Array(2 ** 32 + 1)).toThrowWithMessage(RangeError, "Invalid typed array length");
    expect(() => new Uint16Array(2 ** 31 + 1)).toThrowWithMessage(RangeError, "Invalid typed array length");
    expect(() => new Uint32Array(2 ** 30 + 1)).toThrowWithMessage(RangeError, "Invalid typed array length");
    expect(() => new Float64Array(2 ** 29 + 1)).toThrowWithMessage(RangeError, "Invalid typed array length");
    expect(() => new Uint32Array(2 ** 32)).toThrowWithMessage(RangeError, "Invalid typed array length");
});

test("default species creation preserves constructor length limits", () => {
    [
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
        BigUint64Array,
        BigInt64Array,
    ].forEach(T => {
        const oversizedLength = 2 ** 32 / T.BYTES_PER_ELEMENT + 1;
        expect(() => createDefaultTypedArray(new T(), oversizedLength)).toThrowWithMessage(
            RangeError,
            "Invalid typed array length"
        );
    });
});
