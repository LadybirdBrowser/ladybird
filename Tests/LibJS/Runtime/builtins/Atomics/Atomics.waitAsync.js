describe("errors", () => {
    test("called on non-TypedArray", () => {
        expect(() => {
            Atomics.waitAsync(Symbol.hasInstance, 0, 0, 0);
        }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");
    });

    test("detached buffer", () => {
        expect(() => {
            const typedArray = new Int32Array(4);
            detachArrayBuffer(typedArray.buffer);

            Atomics.waitAsync(typedArray, 0, 0, 0);
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("invalid TypedArray type", () => {
        expect(() => {
            const typedArray = new Float32Array(4);
            Atomics.waitAsync(typedArray, 0, 0, 0);
        }).toThrowWithMessage(TypeError, "Typed array Float32Array element type is not Int32 or BigInt64");
    });

    test("non-shared ArrayBuffer", () => {
        expect(() => {
            const typedArray = new Int32Array(4);
            Atomics.waitAsync(typedArray, 0, 0, 0);
        }).toThrowWithMessage(TypeError, "The array buffer object must be a SharedArrayBuffer");
    });

    test("invalid index", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.waitAsync(typedArray, 4, 0, 0);
        }).toThrowWithMessage(RangeError, "Index 4 is out of range of array length 4");
    });

    test("invalid value", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.waitAsync(typedArray, 0, Symbol.hasInstance, 0);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            const buffer = new SharedArrayBuffer(4 * BigInt64Array.BYTES_PER_ELEMENT);
            const typedArray = new BigInt64Array(buffer);

            Atomics.waitAsync(typedArray, 0, Symbol.hasInstance, 0);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to BigInt");
    });

    test("invalid timeout", () => {
        expect(() => {
            const buffer = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
            const typedArray = new Int32Array(buffer);

            Atomics.waitAsync(typedArray, 0, 0, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

test("basic functionality", () => {
    test("invariants", () => {
        expect(Atomics.waitAsync).toHaveLength(4);
    });
});

for (const TypedArray of [Int32Array, BigInt64Array]) {
    describe(TypedArray.name, () => {
        const zero = TypedArray === BigInt64Array ? 0n : 0;
        const one = TypedArray === BigInt64Array ? 1n : 1;
        const createArray = () => new TypedArray(new SharedArrayBuffer(TypedArray.BYTES_PER_ELEMENT));

        test("a mismatched value returns an immediate result, even with timeout zero", () => {
            const array = createArray();
            expect(Atomics.waitAsync(array, 0, one, 0)).toEqual({ async: false, value: "not-equal" });
            expect(Atomics.notify(array, 0)).toBe(0);
        });

        test("timeout zero returns an immediate result without registering a waiter", () => {
            const array = createArray();
            expect(Atomics.waitAsync(array, 0, zero, 0)).toEqual({ async: false, value: "timed-out" });
            expect(Atomics.notify(array, 0)).toBe(0);
        });

        test("same-agent notification resolves before subsequently queued promise jobs", () => {
            const array = createArray();
            const result = Atomics.waitAsync(array, 0, zero);
            expect(result.async).toBeTrue();
            expect(result.value).toBeInstanceOf(Promise);
            const values = [];
            result.value.then(value => values.push(value));
            expect(Atomics.notify(array, 0, 1)).toBe(1);
            Promise.resolve().then(() => values.push("later promise job"));
            runQueuedPromiseJobs();
            expect(values).toEqual(["ok", "later promise job"]);
            expect(Atomics.notify(array, 0)).toBe(0);
        });

        test("a finite timeout resolves the promise and removes the waiter", () => {
            const array = createArray();
            const result = Atomics.waitAsync(array, 0, zero, 1);
            expect(result.async).toBeTrue();
            expect(result.value).toBeInstanceOf(Promise);
            let value;
            result.value.then(result => {
                value = result;
            });
            runAsyncWaitJobs();
            expect(value).toBe("timed-out");
            expect(Atomics.notify(array, 0)).toBe(0);
        });
    });
}
