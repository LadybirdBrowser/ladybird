const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float16Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.subarray).toHaveLength(2);

        const typedArray = new T(3);
        typedArray[0] = 1;
        typedArray[1] = 2;
        typedArray[2] = 3;

        const subarray = typedArray.subarray(1, 2);
        expect(subarray).toHaveLength(1);
        expect(subarray[0]).toBe(2);
        subarray[0] = 4;
        expect(typedArray[1]).toBe(4);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.subarray).toHaveLength(2);

        const typedArray = new T(3);
        typedArray[0] = 1n;
        typedArray[1] = 2n;
        typedArray[2] = 3n;

        const subarray = typedArray.subarray(1, 2);
        expect(subarray).toHaveLength(1);
        expect(subarray[0]).toBe(2n);
        subarray[0] = 4n;
        expect(typedArray[1]).toBe(4n);
    });
});

test("resizable ArrayBuffer", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
        expect(typedArray.subarray(0, 1).byteLength).toBe(T.BYTES_PER_ELEMENT);

        arrayBuffer.resize(T.BYTES_PER_ELEMENT);
        expect(typedArray.subarray(0, 1).byteLength).toBe(0);
    });
});

test("resizable ArrayBuffer resized during `start` parameter access", () => {
    TYPED_ARRAYS.forEach(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });

        let badAccessor = {
            valueOf: () => {
                arrayBuffer.resize(T.BYTES_PER_ELEMENT * 4);
                return 0;
            },
        };

        let typedArray = new T(arrayBuffer);
        expect(typedArray.subarray(badAccessor, typedArray.length).length).toBe(2);
    });
});

test("default species: result shares the source buffer", () => {
    TYPED_ARRAYS.forEach(T => {
        const source = new T([1, 2, 3, 4, 5]);
        const sub = source.subarray(1, 4);
        expect(sub.buffer).toBe(source.buffer);
        expect(sub.byteOffset).toBe(T.BYTES_PER_ELEMENT);
        expect(sub.length).toBe(3);
        expect(sub.constructor).toBe(T);
        sub[0] = 9;
        expect(source[1]).toBe(9);
    });
});

test("Symbol.species returns the default constructor", () => {
    TYPED_ARRAYS.forEach(T => {
        class Subclass extends T {
            static get [Symbol.species]() {
                return T;
            }
        }
        const source = new Subclass([1, 2, 3, 4, 5]);
        const sub = source.subarray(1, 4);
        expect(sub.buffer).toBe(source.buffer);
        expect(sub.constructor).toBe(T);
        expect(Object.getPrototypeOf(sub)).toBe(T.prototype);
        expect(sub.length).toBe(3);
    });
});

test("Symbol.species returns a different typed array constructor", () => {
    TYPED_ARRAYS.forEach(T => {
        class Subclass extends T {
            static get [Symbol.species]() {
                return Uint8Array;
            }
        }
        const source = new Subclass(new ArrayBuffer(T.BYTES_PER_ELEMENT * 4));
        const sub = source.subarray(1, 3);
        expect(sub.buffer).toBe(source.buffer);
        expect(sub.constructor).toBe(Uint8Array);
        expect(sub.byteOffset).toBe(T.BYTES_PER_ELEMENT);
        expect(sub.length).toBe(2);
    });
});

test("buffer detached during species lookup throws TypeError", () => {
    TYPED_ARRAYS.forEach(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const source = new T(buffer);
        class Subclass extends T {
            static get [Symbol.species]() {
                buffer.transfer();
                return T;
            }
        }
        Object.setPrototypeOf(source, Subclass.prototype);
        expect(() => source.subarray(0, 2)).toThrowWithMessage(TypeError, "ArrayBuffer is detached");
    });
});

test("buffer shrunk below begin offset during species lookup throws RangeError", () => {
    TYPED_ARRAYS.forEach(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });
        const source = new T(buffer);
        class Subclass extends T {
            static get [Symbol.species]() {
                buffer.resize(0);
                return T;
            }
        }
        Object.setPrototypeOf(source, Subclass.prototype);
        expect(() => source.subarray(2, 4)).toThrow(RangeError);
    });
});

test("auto-length view: buffer shrunk below begin offset during species lookup throws RangeError", () => {
    TYPED_ARRAYS.forEach(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });
        const source = new T(buffer);
        class Subclass extends T {
            static get [Symbol.species]() {
                buffer.resize(0);
                return T;
            }
        }
        Object.setPrototypeOf(source, Subclass.prototype);
        // No `end` argument: source is auto-length, so subarray builds an auto-length view.
        expect(() => source.subarray(2)).toThrow(RangeError);
    });
});

test("subclass without overriding Symbol.species invokes the subclass constructor", () => {
    TYPED_ARRAYS.forEach(T => {
        let constructorCalls = [];
        class Subclass extends T {
            constructor(...args) {
                super(...args);
                constructorCalls.push(args.length);
            }
        }
        const source = new Subclass([1, 2, 3, 4, 5]);
        constructorCalls = [];
        const sub = source.subarray(1, 4);
        expect(sub).toBeInstanceOf(Subclass);
        expect(sub.buffer).toBe(source.buffer);
        expect(sub.length).toBe(3);
        expect(constructorCalls).toEqual([3]);
    });
});
