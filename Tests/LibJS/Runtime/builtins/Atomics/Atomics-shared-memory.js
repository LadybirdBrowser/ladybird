describe("Atomics on shared memory", () => {
    // The read-modify-write ops take a different path for an SAB: A compare-exchange loop against the live element —
    // rather than reading the element out, modifying that copy and writing it back. From one agent, the two must be
    // indistinguishable: each op returns the old value and leaves the new one in place. So, that's what these check.
    const INTEGER_TYPES = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];

    const sharedOf = (T, initial) => {
        const array = new T(new SharedArrayBuffer(T.BYTES_PER_ELEMENT * 2));
        array[0] = initial;
        return array;
    };

    test("add returns the old value and stores the sum", () => {
        INTEGER_TYPES.forEach(T => {
            const array = sharedOf(T, 5);
            expect(Atomics.add(array, 0, 3)).toBe(5);
            expect(array[0]).toBe(8);
        });
    });

    test("sub returns the old value and stores the difference", () => {
        INTEGER_TYPES.forEach(T => {
            const array = sharedOf(T, 9);
            expect(Atomics.sub(array, 0, 4)).toBe(9);
            expect(array[0]).toBe(5);
        });
    });

    test("bitwise operations return the old value and store the result", () => {
        INTEGER_TYPES.forEach(T => {
            expect(Atomics.and(sharedOf(T, 0b1100), 0, 0b1010)).toBe(0b1100);
            expect(Atomics.or(sharedOf(T, 0b1100), 0, 0b1010)).toBe(0b1100);
            expect(Atomics.xor(sharedOf(T, 0b1100), 0, 0b1010)).toBe(0b1100);

            const anded = sharedOf(T, 0b1100);
            Atomics.and(anded, 0, 0b1010);
            expect(anded[0]).toBe(0b1000);

            const ored = sharedOf(T, 0b1100);
            Atomics.or(ored, 0, 0b1010);
            expect(ored[0]).toBe(0b1110);

            const xored = sharedOf(T, 0b1100);
            Atomics.xor(xored, 0, 0b1010);
            expect(xored[0]).toBe(0b0110);
        });
    });

    test("exchange returns the old value and stores the new one", () => {
        INTEGER_TYPES.forEach(T => {
            const array = sharedOf(T, 7);
            expect(Atomics.exchange(array, 0, 2)).toBe(7);
            expect(array[0]).toBe(2);
        });
    });

    test("compareExchange only stores when the element matches", () => {
        INTEGER_TYPES.forEach(T => {
            const array = sharedOf(T, 4);
            expect(Atomics.compareExchange(array, 0, 99, 6)).toBe(4);
            expect(array[0]).toBe(4);

            expect(Atomics.compareExchange(array, 0, 4, 6)).toBe(4);
            expect(array[0]).toBe(6);
        });
    });

    test("load and store round-trip", () => {
        INTEGER_TYPES.forEach(T => {
            const array = sharedOf(T, 0);
            expect(Atomics.store(array, 0, 3)).toBe(3);
            expect(Atomics.load(array, 0)).toBe(3);
        });
    });

    test("operations at a non-zero index", () => {
        // The element the loop compares and exchanges is the one at the given index — not the buffer's first.
        const array = new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT * 4));
        array[2] = 10;
        expect(Atomics.add(array, 2, 5)).toBe(10);
        expect(array[2]).toBe(15);
        expect(array[0]).toBe(0);
        expect(array[3]).toBe(0);
    });

    test("BigInt element types", () => {
        [BigInt64Array, BigUint64Array].forEach(T => {
            const array = new T(new SharedArrayBuffer(T.BYTES_PER_ELEMENT * 2));
            array[0] = 5n;
            expect(Atomics.add(array, 0, 3n)).toBe(5n);
            expect(array[0]).toBe(8n);
            expect(Atomics.exchange(array, 0, 1n)).toBe(8n);
            expect(array[0]).toBe(1n);
        });
    });
});
