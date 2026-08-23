describe("SharedArrayBuffer element access", () => {
    // Shared-buffer reads/writes go down a different path than unshared ones: Where the memory model forbids tearing,
    // the element is loaded+stored with a single atomic instruction, instead of a byte copy. That's not observable from
    // one agent; but the values it yields must be identical either way — for every element type and at every alignment.
    const TYPED_ARRAYS = [
        Int8Array,
        Uint8Array,
        Uint8ClampedArray,
        Int16Array,
        Uint16Array,
        Int32Array,
        Uint32Array,
        Float32Array,
        Float64Array,
    ];
    const BIGINT_TYPED_ARRAYS = [BigInt64Array, BigUint64Array];

    test("every element type round-trips through shared memory", () => {
        TYPED_ARRAYS.forEach(T => {
            const shared = new T(new SharedArrayBuffer(T.BYTES_PER_ELEMENT * 4));
            const unshared = new T(new ArrayBuffer(T.BYTES_PER_ELEMENT * 4));

            for (let i = 0; i < shared.length; ++i) {
                shared[i] = i + 1;
                unshared[i] = i + 1;
            }

            expect(shared[0]).toBe(unshared[0]);
            expect(shared[shared.length - 1]).toBe(unshared[unshared.length - 1]);
            expect(Array.from(shared)).toEqual(Array.from(unshared));
        });
    });

    test("BigInt element types round-trip through shared memory", () => {
        BIGINT_TYPED_ARRAYS.forEach(T => {
            const shared = new T(new SharedArrayBuffer(T.BYTES_PER_ELEMENT * 2));
            shared[0] = 1n;
            shared[1] = 2n;

            expect(shared[0]).toBe(1n);
            expect(shared[1]).toBe(2n);
        });
    });

    test("DataView accesses shared memory at any offset", () => {
        // A DataView may sit at an offset that's not naturally aligned — which no atomic instruction can address. Those
        // fall back to the byte copy — so they must still read and write correctly.
        const view = new DataView(new SharedArrayBuffer(16));

        for (const offset of [0, 1, 2, 3, 7]) {
            view.setInt32(offset, -123456 + offset);
            expect(view.getInt32(offset)).toBe(-123456 + offset);

            view.setFloat64(offset, 1.5 + offset);
            expect(view.getFloat64(offset)).toBe(1.5 + offset);
        }
    });

    test("shared and unshared views agree on endianness", () => {
        const shared = new DataView(new SharedArrayBuffer(4));
        const unshared = new DataView(new ArrayBuffer(4));

        shared.setUint32(0, 0x01020304, true);
        unshared.setUint32(0, 0x01020304, true);
        expect(shared.getUint32(0, false)).toBe(unshared.getUint32(0, false));
        expect(new Uint8Array(shared.buffer)[0]).toBe(0x04);
    });
});
