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
        expect(T.prototype.forEach).toHaveLength(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.forEach).toHaveLength(1);
    });
});

describe("errors", () => {
    function errorTests(T) {
        test(`requires at least one argument (${T.name})`, () => {
            expect(() => {
                new T().forEach();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray.prototype.forEach() requires at least one argument"
            );
        });

        test(`callback must be a function (${T.name})`, () => {
            expect(() => {
                new T().forEach(undefined);
            }).toThrowWithMessage(TypeError, "undefined is not a function");
        });

        test(`ArrayBuffer out of bounds (${T.name})`, () => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.forEach(() => {});
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
    test("never calls callback with empty array", () => {
        function emptyTest(T) {
            var callbackCalled = 0;
            expect(
                new T().forEach(() => {
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
                new T([1, 2, 3]).forEach(() => {
                    callbackCalled++;
                })
            ).toBeUndefined();
            expect(callbackCalled).toBe(3);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            var callbackCalled = 0;
            expect(
                new T([1n, 2n, 3n]).forEach(() => {
                    callbackCalled++;
                })
            ).toBeUndefined();
            expect(callbackCalled).toBe(3);
        });
    });

    test("callback receives value and index", () => {
        TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1, 2, 3]);
            typedArray.forEach((value, index) => {
                expect(value).toBe(typedArray[index]);
                expect(index).toBe(typedArray[index] - 1);
            });
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1n, 2n, 3n]);
            typedArray.forEach((value, index) => {
                expect(value).toBe(typedArray[index]);
                expect(index).toBe(Number(typedArray[index] - 1n));
            });
        });
    });

    test("callback receives array", () => {
        TYPED_ARRAYS.forEach(T => {
            var callbackCalled = 0;
            const typedArray = new T([1, 2, 3]);
            typedArray.forEach((_, __, array) => {
                callbackCalled++;
                expect(typedArray).toEqual(array);
            });
            expect(callbackCalled).toBe(3);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            var callbackCalled = 0;
            const typedArray = new T([1n, 2n, 3n]);
            typedArray.forEach((_, __, array) => {
                callbackCalled++;
                expect(typedArray).toEqual(array);
            });
            expect(callbackCalled).toBe(3);
        });
    });

    test("this value can be modified", () => {
        TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1, 2, 3]);
            [4, 5, 6].forEach(function (value, index) {
                this[index] = value;
            }, typedArray);
            expect(typedArray[0]).toBe(4);
            expect(typedArray[1]).toBe(5);
            expect(typedArray[2]).toBe(6);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const typedArray = new T([1n, 2n, 3n]);
            [4n, 5n, 6n].forEach(function (value, index) {
                this[index] = value;
            }, typedArray);
            expect(typedArray[0]).toBe(4n);
            expect(typedArray[1]).toBe(5n);
            expect(typedArray[2]).toBe(6n);
        });
    });
});
