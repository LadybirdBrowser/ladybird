test("invariants", () => {
    expect(Atomics.add).toHaveLength(3);
});

test("error cases", () => {
    expect(() => {
        Atomics.add("not an array", 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.add(bad_array_type, 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.add(bad_array_type, 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.add(array, 100, 1);
    }).toThrow(RangeError);
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(4);

        expect(Atomics.add(array, 0, 1)).toBe(0);
        expect(array).toEqual(new ArrayType([1, 0, 0, 0]));

        expect(Atomics.add(array, 0, 1)).toBe(1);
        expect(array).toEqual(new ArrayType([2, 0, 0, 0]));

        expect(Atomics.add(array, 2, 3.14)).toBe(0);
        expect(array).toEqual(new ArrayType([2, 0, 3, 0]));

        expect(Atomics.add(array, 3, "1")).toBe(0);
        expect(array).toEqual(new ArrayType([2, 0, 3, 1]));
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(4);

        expect(Atomics.add(array, 0, 1n)).toBe(0n);
        expect(array).toEqual(new ArrayType([1n, 0n, 0n, 0n]));

        expect(Atomics.add(array, 0, 1n)).toBe(1n);
        expect(array).toEqual(new ArrayType([2n, 0n, 0n, 0n]));

        expect(Atomics.add(array, 2, 3n)).toBe(0n);
        expect(array).toEqual(new ArrayType([2n, 0n, 3n, 0n]));

        expect(Atomics.add(array, 3, 4n)).toBe(0n);
        expect(array).toEqual(new ArrayType([2n, 0n, 3n, 4n]));
    });
});
