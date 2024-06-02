test("invariants", () => {
    expect(Atomics.exchange).toHaveLength(3);
});

test("error cases", () => {
    expect(() => {
        Atomics.exchange("not an array", 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.exchange(bad_array_type, 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.exchange(bad_array_type, 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.exchange(array, 100, 0);
    }).toThrow(RangeError);
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1;
        array[1] = 2;
        array[2] = 3;
        array[3] = 4;

        expect(Atomics.exchange(array, 0, 5)).toBe(1);
        expect(array).toEqual(new ArrayType([5, 2, 3, 4]));

        expect(Atomics.exchange(array, 0, 6)).toBe(5);
        expect(array).toEqual(new ArrayType([6, 2, 3, 4]));

        expect(Atomics.exchange(array, "1", 7)).toBe(2);
        expect(array).toEqual(new ArrayType([6, 7, 3, 4]));

        expect(Atomics.exchange(array, 2, "8")).toBe(3);
        expect(array).toEqual(new ArrayType([6, 7, 8, 4]));

        expect(Atomics.exchange(array, 3.14, 9)).toBe(4);
        expect(array).toEqual(new ArrayType([6, 7, 8, 9]));
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(4);
        array[0] = 1n;
        array[1] = 2n;
        array[2] = 3n;
        array[3] = 4n;

        expect(Atomics.exchange(array, 0, 5n)).toBe(1n);
        expect(array).toEqual(new ArrayType([5n, 2n, 3n, 4n]));

        expect(Atomics.exchange(array, 0, 6n)).toBe(5n);
        expect(array).toEqual(new ArrayType([6n, 2n, 3n, 4n]));

        expect(Atomics.exchange(array, 1, 7n)).toBe(2n);
        expect(array).toEqual(new ArrayType([6n, 7n, 3n, 4n]));

        expect(Atomics.exchange(array, 2, 8n)).toBe(3n);
        expect(array).toEqual(new ArrayType([6n, 7n, 8n, 4n]));

        expect(Atomics.exchange(array, 3, 9n)).toBe(4n);
        expect(array).toEqual(new ArrayType([6n, 7n, 8n, 9n]));
    });
});
