test("invariants", () => {
    expect(Atomics.sub).toHaveLength(3);
});

test("error cases", () => {
    expect(() => {
        Atomics.sub("not an array", 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.sub(bad_array_type, 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.sub(bad_array_type, 0, 1);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.sub(array, 100, 1);
    }).toThrow(RangeError);
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1;
        array[1] = 2;
        array[2] = 3;
        array[3] = 4;

        expect(Atomics.sub(array, 0, 1)).toBe(1);
        expect(array).toEqual(new ArrayType([0, 2, 3, 4]));

        expect(Atomics.sub(array, 1, 1)).toBe(2);
        expect(array).toEqual(new ArrayType([0, 1, 3, 4]));

        expect(Atomics.sub(array, 1, 1)).toBe(1);
        expect(array).toEqual(new ArrayType([0, 0, 3, 4]));

        expect(Atomics.sub(array, 2, 3.14)).toBe(3);
        expect(array).toEqual(new ArrayType([0, 0, 0, 4]));

        expect(Atomics.sub(array, 3, "1")).toBe(4);
        expect(array).toEqual(new ArrayType([0, 0, 0, 3]));
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1n;
        array[1] = 2n;
        array[2] = 3n;
        array[3] = 4n;

        expect(Atomics.sub(array, 0, 1n)).toBe(1n);
        expect(array).toEqual(new ArrayType([0n, 2n, 3n, 4n]));

        expect(Atomics.sub(array, 1, 1n)).toBe(2n);
        expect(array).toEqual(new ArrayType([0n, 1n, 3n, 4n]));

        expect(Atomics.sub(array, 1, 1n)).toBe(1n);
        expect(array).toEqual(new ArrayType([0n, 0n, 3n, 4n]));

        expect(Atomics.sub(array, 2, 3n)).toBe(3n);
        expect(array).toEqual(new ArrayType([0n, 0n, 0n, 4n]));

        expect(Atomics.sub(array, 3, 1n)).toBe(4n);
        expect(array).toEqual(new ArrayType([0n, 0n, 0n, 3n]));
    });
});
