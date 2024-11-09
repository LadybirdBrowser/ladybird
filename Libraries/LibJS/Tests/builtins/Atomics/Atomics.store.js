test("invariants", () => {
    expect(Atomics.store).toHaveLength(3);
});

test("error cases", () => {
    expect(() => {
        Atomics.store("not an array", 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.store(bad_array_type, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.store(bad_array_type, 0);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.store(array, 100);
    }).toThrow(RangeError);
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(4);

        expect(Atomics.store(array, 0, 1)).toBe(1);
        expect(array[0]).toBe(1);

        expect(Atomics.store(array, 1, 2)).toBe(2);
        expect(array[1]).toBe(2);

        expect(Atomics.store(array, 2, 3)).toBe(3);
        expect(array[2]).toBe(3);

        expect(Atomics.store(array, 3, 4)).toBe(4);
        expect(array[3]).toBe(4);
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(4);

        expect(Atomics.store(array, 0, 1n)).toBe(1n);
        expect(array[0]).toBe(1n);

        expect(Atomics.store(array, 1, 2n)).toBe(2n);
        expect(array[1]).toBe(2n);

        expect(Atomics.store(array, 2, 3n)).toBe(3n);
        expect(array[2]).toBe(3n);

        expect(Atomics.store(array, 3, 4n)).toBe(4n);
        expect(array[3]).toBe(4n);
    });
});
