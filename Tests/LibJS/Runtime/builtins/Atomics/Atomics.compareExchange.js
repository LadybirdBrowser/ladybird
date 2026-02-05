test("invariants", () => {
    expect(Atomics.compareExchange).toHaveLength(4);
});

test("error cases", () => {
    expect(() => {
        Atomics.compareExchange("not an array", 0, 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Float32Array(4);
        Atomics.compareExchange(bad_array_type, 0, 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const bad_array_type = new Uint8ClampedArray(4);
        Atomics.compareExchange(bad_array_type, 0, 0, 0);
    }).toThrow(TypeError);

    expect(() => {
        const array = new Int32Array(4);
        Atomics.compareExchange(array, 100, 0, 0);
    }).toThrow(RangeError);

    expect(() => {
        const array = new Int32Array(4);

        function detachArrayWhileAccessingIndex(array) {
            return {
                valueOf() {
                    detachArrayBuffer(array.buffer);
                    return 0;
                },
            };
        }

        Atomics.compareExchange(array, detachArrayWhileAccessingIndex(array), 0, 0);
    }).toThrowWithMessage(
        TypeError,
        "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
    );
});

test("basic functionality (non-BigInt)", () => {
    [Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array].forEach(ArrayType => {
        const array = new ArrayType(2);
        array[0] = 1;
        array[1] = 2;

        expect(Atomics.compareExchange(array, 0, 0, 5)).toBe(1);
        expect(array).toEqual(new ArrayType([1, 2]));

        expect(Atomics.compareExchange(array, 0, 1, "5")).toBe(1);
        expect(array).toEqual(new ArrayType([5, 2]));

        expect(Atomics.compareExchange(array, 0, "5", 6)).toBe(5);
        expect(array).toEqual(new ArrayType([6, 2]));

        expect(Atomics.compareExchange(array, 1, 2, 3.14)).toBe(2);
        expect(array).toEqual(new ArrayType([6, 3]));
    });
});

test("basic functionality (BigInt)", () => {
    [BigInt64Array, BigUint64Array].forEach(ArrayType => {
        const array = new ArrayType(2);
        array[0] = 1n;
        array[1] = 2n;

        expect(Atomics.compareExchange(array, 0, 0n, 5n)).toBe(1n);
        expect(array).toEqual(new ArrayType([1n, 2n]));

        expect(Atomics.compareExchange(array, 0, 1n, 5n)).toBe(1n);
        expect(array).toEqual(new ArrayType([5n, 2n]));

        expect(Atomics.compareExchange(array, 0, 5n, 6n)).toBe(5n);
        expect(array).toEqual(new ArrayType([6n, 2n]));

        expect(Atomics.compareExchange(array, 1, 2n, 3n)).toBe(2n);
        expect(array).toEqual(new ArrayType([6n, 3n]));
    });
});
