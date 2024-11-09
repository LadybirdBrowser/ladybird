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
        expect(T.prototype.map).toHaveLength(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.map).toHaveLength(1);
    });
});

describe("errors", () => {
    function argumentErrorTests(T) {
        test(`requires at least one argument (${T.name})`, () => {
            expect(() => {
                new T().map();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray.prototype.map() requires at least one argument"
            );
        });

        test(`callback must be a function (${T.name})`, () => {
            expect(() => {
                new T().map(undefined);
            }).toThrowWithMessage(TypeError, "undefined is not a function");
        });

        test(`ArrayBuffer out of bounds (${T.name})`, () => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.map(value => value);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    }

    TYPED_ARRAYS.forEach(T => argumentErrorTests(T));
    BIGINT_TYPED_ARRAYS.forEach(T => argumentErrorTests(T));

    test("throws if mappedValue is not the same type of the typed array", () => {
        TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            let result;

            expect(() => {
                result = new T([1, 2, 3]).map((value, index) => {
                    callbackCalled++;
                    return index % 2 === 0 ? 1n : 1;
                });
            }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

            expect(callbackCalled).toBe(1);
            expect(result).toBeUndefined();
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            let result;

            expect(() => {
                result = new T([1n, 2n, 3n]).map((value, index) => {
                    callbackCalled++;
                    return index % 2 === 0 ? 1 : 1n;
                });
            }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");

            expect(callbackCalled).toBe(1);
            expect(result).toBeUndefined();
        });
    });

    test("Symbol.species returns a typed array with a different content type", () => {
        TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return BigUint64Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray().map(() => {});
            }).toThrowWithMessage(TypeError, `Can't create BigUint64Array from ${T.name}`);

            expect(result).toBeUndefined();
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return Uint32Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray().map(() => {});
            }).toThrowWithMessage(TypeError, `Can't create Uint32Array from ${T.name}`);

            expect(result).toBeUndefined();
        });
    });

    test("Symbol.species doesn't return a typed array", () => {
        TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray().map(() => {});
            }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");

            expect(result).toBeUndefined();
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray().map(() => {});
            }).toThrowWithMessage(TypeError, "Not an object of type TypedArray");

            expect(result).toBeUndefined();
        });
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty array", () => {
        TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            expect(
                new T([]).map(() => {
                    callbackCalled++;
                })
            ).toHaveLength(0);
            expect(callbackCalled).toBe(0);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            expect(
                new T([]).map(() => {
                    callbackCalled++;
                })
            ).toHaveLength(0);
            expect(callbackCalled).toBe(0);
        });
    });

    test("calls callback once for every item", () => {
        TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            expect(
                new T([1, 2, 3]).map(value => {
                    callbackCalled++;
                    // NOTE: This is just to prevent a conversion exception.
                    return value;
                })
            ).toHaveLength(3);
            expect(callbackCalled).toBe(3);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            let callbackCalled = 0;
            expect(
                new T([1n, 2n, 3n]).map(value => {
                    callbackCalled++;
                    // NOTE: This is just to prevent a conversion exception.
                    return value;
                })
            ).toHaveLength(3);
            expect(callbackCalled).toBe(3);
        });
    });

    test("can map based on callback return value", () => {
        TYPED_ARRAYS.forEach(T => {
            const squaredNumbers = new T([0, 1, 2, 3, 4]).map(x => x ** 2);
            expect(squaredNumbers).toHaveLength(5);
            expect(squaredNumbers[0]).toBe(0);
            expect(squaredNumbers[1]).toBe(1);
            expect(squaredNumbers[2]).toBe(4);
            expect(squaredNumbers[3]).toBe(9);
            expect(squaredNumbers[4]).toBe(16);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            const squaredNumbers = new T([0n, 1n, 2n, 3n, 4n]).map(x => x ** 2n);
            expect(squaredNumbers).toHaveLength(5);
            expect(squaredNumbers[0]).toBe(0n);
            expect(squaredNumbers[1]).toBe(1n);
            expect(squaredNumbers[2]).toBe(4n);
            expect(squaredNumbers[3]).toBe(9n);
            expect(squaredNumbers[4]).toBe(16n);
        });
    });

    test("Symbol.species returns a typed array with a matching content type", () => {
        TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return Uint32Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray([1, 2, 3]).map(value => value + 2);
            }).not.toThrowWithMessage(TypeError, `Can't create Uint32Array from ${T.name}`);

            expect(result).toBeInstanceOf(Uint32Array);
            expect(result).toHaveLength(3);
            expect(result[0]).toBe(3);
            expect(result[1]).toBe(4);
            expect(result[2]).toBe(5);
        });

        BIGINT_TYPED_ARRAYS.forEach(T => {
            class TypedArray extends T {
                static get [Symbol.species]() {
                    return BigUint64Array;
                }
            }

            let result;

            expect(() => {
                result = new TypedArray([1n, 2n, 3n]).map(value => value + 2n);
            }).not.toThrowWithMessage(TypeError, `Can't create BigUint64Array from ${T.name}`);

            expect(result).toBeInstanceOf(BigUint64Array);
            expect(result).toHaveLength(3);
            expect(result[0]).toBe(3n);
            expect(result[1]).toBe(4n);
            expect(result[2]).toBe(5n);
        });
    });
});
