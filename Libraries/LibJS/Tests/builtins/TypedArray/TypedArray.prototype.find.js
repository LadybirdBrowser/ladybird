const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

test("length is 1", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.find).toHaveLength(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.find).toHaveLength(1);
    });
});

describe("errors", () => {
    function errorTests(T) {
        test(`requires at least one argument (${T.name})`, () => {
            expect(() => {
                new T().find();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray.prototype.find() requires at least one argument"
            );
        });

        test(`callback must be a function (${T.name})`, () => {
            expect(() => {
                new T().find(undefined);
            }).toThrowWithMessage(TypeError, "undefined is not a function");
        });

        test(`ArrayBuffer out of bounds (${T.name})`, () => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.find(value => value === 0);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    }

    TYPED_ARRAYS.forEach(T => errorTests(T));
    BIGINT_TYPED_ARRAYS.forEach(T => errorTests(T));
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1, 2, 3]);

            expect(typedArray.find(value => value === 1)).toBe(1);
            expect(typedArray.find(value => value === 2)).toBe(2);
            expect(typedArray.find(value => value === 3)).toBe(3);
            expect(typedArray.find((value, index) => index === 1)).toBe(2);
            expect(typedArray.find(value => value == "1")).toBe(1);
            expect(typedArray.find(value => value === 10)).toBeUndefined();

            const typedArrayDuplicates = new T([2, 1, 2, 3, 1]);

            expect(typedArrayDuplicates.find(value => value < 3)).toBe(2);
            expect(typedArrayDuplicates.find(value => value < 2)).toBe(1);
            expect(typedArrayDuplicates.find(value => value > 1)).toBe(2);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1n, 2n, 3n]);

            expect(typedArray.find(value => value === 1n)).toBe(1n);
            expect(typedArray.find(value => value === 2n)).toBe(2n);
            expect(typedArray.find(value => value === 3n)).toBe(3n);
            expect(typedArray.find((value, index) => index === 1)).toBe(2n);
            expect(typedArray.find(value => value == 1)).toBe(1n);
            expect(typedArray.find(value => value == "1")).toBe(1n);
            expect(typedArray.find(value => value === 1)).toBeUndefined();

            const typedArrayDuplicates = new T([2n, 1n, 2n, 3n, 1n]);

            expect(typedArrayDuplicates.find(value => value < 3)).toBe(2n);
            expect(typedArrayDuplicates.find(value => value < 2)).toBe(1n);
            expect(typedArrayDuplicates.find(value => value > 1)).toBe(2n);
        });
    });

    test("never calls callback with empty array", () => {
        function emptyTest(T) {
            var callbackCalled = 0;
            expect(
                new T().find(() => {
                    callbackCalled++;
                })
            ).toBeUndefined();
            expect(callbackCalled).toBe(0);
        }

        TYPED_ARRAYS.forEach(T => emptyTest(T));
        BIGINT_TYPED_ARRAYS.forEach(T => emptyTest(T));
    });

    test("calls callback once for every item", () => {
        TYPED_ARRAYS.forEach(T => {
            var callbackCalled = 0;
            expect(
                new T([1, 2, 3]).find(() => {
                    callbackCalled++;
                })
            ).toBeUndefined();
            expect(callbackCalled).toBe(3);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            var callbackCalled = 0;
            expect(
                new T([1n, 2n, 3n]).find(() => {
                    callbackCalled++;
                })
            ).toBeUndefined();
            expect(callbackCalled).toBe(3);
        });
    });
});
