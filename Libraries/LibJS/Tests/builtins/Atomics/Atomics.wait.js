describe("errors", () => {
    test("called on non-TypedArray", () => {
        expect(() => {
            Atomics.wait(Symbol.hasInstance, 0, 0, 0);
        }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");
    });

    test("detached buffer", () => {
        expect(() => {
            const typedArray = new Int32Array(4);
            detachArrayBuffer(typedArray.buffer);

            Atomics.wait(typedArray, 0, 0, 0);
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("invalid TypedArray type", () => {
        expect(() => {
            const typedArray = new Float32Array(4);
            Atomics.wait(typedArray, 0, 0, 0);
        }).toThrowWithMessage(
            TypeError,
            "Typed array Float32Array element type is not Int32 or BigInt64"
        );
    });

    test("non-shared ArrayBuffer", () => {
        expect(() => {
            const typedArray = new Int32Array(4);
            Atomics.wait(typedArray, 0, 0, 0);
        }).toThrowWithMessage(TypeError, "The array buffer object must be a SharedArrayBuffer");
    });

    test("invalid index", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.wait(typedArray, 4, 0, 0);
        }).toThrowWithMessage(RangeError, "Index 4 is out of range of array length 4");
    });

    test("invalid value", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.wait(typedArray, 0, Symbol.hasInstance, 0);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            const buffer = new SharedArrayBuffer(4 * BigInt64Array.BYTES_PER_ELEMENT);
            const typedArray = new BigInt64Array(buffer);

            Atomics.wait(typedArray, 0, Symbol.hasInstance, 0);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to BigInt");
    });

    test("invalid timeout", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.wait(typedArray, 0, 0, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

test("basic functionality", () => {
    test("invariants", () => {
        expect(Atomics.wait).toHaveLength(4);
    });
});
