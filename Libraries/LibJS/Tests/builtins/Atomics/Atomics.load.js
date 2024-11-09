test("invariants", () => {
    expect(Atomics.load).toHaveLength(2);
});

test("error cases", () => {
    expect(() => {
        Atomics.load("not an array", 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.load(bad_array_type, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.load(bad_array_type, 0);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.load(array, 100);
    }).toThrow(RangeError);
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1;
        array[1] = 2;
        array[2] = 3;
        array[3] = 4;

        expect(Atomics.load(array, 0)).toBe(1);
        expect(Atomics.load(array, 1)).toBe(2);
        expect(Atomics.load(array, 2)).toBe(3);
        expect(Atomics.load(array, 3)).toBe(4);
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1n;
        array[1] = 2n;
        array[2] = 3n;
        array[3] = 4n;

        expect(Atomics.load(array, 0)).toBe(1n);
        expect(Atomics.load(array, 1)).toBe(2n);
        expect(Atomics.load(array, 2)).toBe(3n);
        expect(Atomics.load(array, 3)).toBe(4n);
    });
});
