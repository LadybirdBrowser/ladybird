test("basic functionality", () => {
    expect(Uint8Array.BYTES_PER_ELEMENT).toBe(1);
    expect(Uint8ClampedArray.BYTES_PER_ELEMENT).toBe(1);
    expect(Uint16Array.BYTES_PER_ELEMENT).toBe(2);
    expect(Uint32Array.BYTES_PER_ELEMENT).toBe(4);
    expect(BigUint64Array.BYTES_PER_ELEMENT).toBe(8);
    expect(Int8Array.BYTES_PER_ELEMENT).toBe(1);
    expect(Int16Array.BYTES_PER_ELEMENT).toBe(2);
    expect(Int32Array.BYTES_PER_ELEMENT).toBe(4);
    expect(BigInt64Array.BYTES_PER_ELEMENT).toBe(8);
    expect(Float32Array.BYTES_PER_ELEMENT).toBe(4);
    expect(Float64Array.BYTES_PER_ELEMENT).toBe(8);
});
