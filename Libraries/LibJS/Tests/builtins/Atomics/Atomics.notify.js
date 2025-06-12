describe("errors", () => {
    test("called on non-TypedArray", () => {
        expect(() => {
            Atomics.notify(Symbol.hasInstance, 0, 0);
        }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");
    });

    test("detached buffer", () => {
        expect(() => {
            const typedArray = new Int32Array(4);
            detachArrayBuffer(typedArray.buffer);

            Atomics.notify(typedArray, 0, 0);
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("invalid TypedArray type", () => {
        expect(() => {
            const typedArray = new Float32Array(4);
            Atomics.notify(typedArray, 0, 0);
        }).toThrowWithMessage(
            TypeError,
            "Typed array Float32Array element type is not Int32 or BigInt64"
        );
    });

    test("invalid index", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.notify(typedArray, 4, 0);
        }).toThrowWithMessage(RangeError, "Index 4 is out of range of array length 4");
    });

    test("invalid count", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.notify(typedArray, 0, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

test("basic functionality", () => {
    test("invariants", () => {
        expect(Atomics.notify).toHaveLength(3);
    });

    test("non-shared ArrayBuffer", () => {
        const typedArray = new Int32Array(4);
        const waiters = Atomics.notify(typedArray, 0, 0);
        expect(waiters).toBe(0);
    });
});
