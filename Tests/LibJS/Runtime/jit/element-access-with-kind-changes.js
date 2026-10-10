// Indexed gets and puts, compiled after warming up with packed arrays of int32 values and in-bounds indices, then given
// every other kind of object and index: holes, out-of-bounds and negative indices, doubles, strings, every typed
// array kind, detached buffers, frozen arrays, arrays with accessors, proxies and array-likes. The compiled code must
// do what the interpreter does (see compareCompiledWithInterpreter in test-common.js).

const objects = {
    packed: (log, i) => [i, 1, 2, 3],
    "packed doubles": (log, i) => [i + 0.5, 1.5, 2.5, 3.5],
    "packed mixed": (log, i) => [i, "one", { two: 2 }, null],
    holey: (log, i) => [i, , 2, , 4],
    "holey with prototype element": (log, i) => {
        const array = [i, , 2, , 4];
        Object.setPrototypeOf(array, Object.assign(Object.create(Array.prototype), { 1: "from prototype" }));
        return array;
    },
    "with getter element": (log, i) =>
        Object.defineProperty([i, 1, 2, 3], 1, {
            get() {
                log.push("element getter");
                return "got";
            },
            set(value) {
                log.push(`element setter ${value}`);
            },
        }),
    frozen: (log, i) => Object.freeze([i, 1, 2, 3]),
    "non-extensible": (log, i) => Object.preventExtensions([i, 1]),
    empty: () => [],
    long: (log, i) => new Array(100).fill(i),
    "length changed": (log, i) => {
        const array = [i, 1, 2, 3];
        array.length = 1;
        return array;
    },
    sparse: (log, i) => {
        const array = [];
        array[1000] = i;
        return array;
    },
    Int8Array: (log, i) => new Int8Array([i, 127, -128, 3]),
    Uint8Array: (log, i) => new Uint8Array([i, 255, 0, 3]),
    Uint8ClampedArray: (log, i) => new Uint8ClampedArray([i, 255, 0, 3]),
    Int16Array: (log, i) => new Int16Array([i, -32768, 32767, 3]),
    Uint16Array: (log, i) => new Uint16Array([i, 65535, 0, 3]),
    Int32Array: (log, i) => new Int32Array([i, -2147483648, 2147483647, 3]),
    Uint32Array: (log, i) => new Uint32Array([i, 4294967295, 2147483648, 3]),
    Float16Array: (log, i) => new Float16Array([i, 0.5, -0, 3]),
    Float32Array: (log, i) => new Float32Array([i, 0.1, NaN, -0]),
    Float64Array: (log, i) => new Float64Array([i, 0.1, NaN, -0]),
    BigInt64Array: (log, i) => new BigInt64Array([BigInt(i), -1n, 2n, 3n]),
    BigUint64Array: (log, i) => new BigUint64Array([BigInt(i), 1n, 2n, 3n]),
    "detached typed array": () => {
        const array = new Int32Array(4);
        detachArrayBuffer(array.buffer);
        return array;
    },
    "resizable typed array": (log, i) => {
        const buffer = new ArrayBuffer(16, { maxByteLength: 64 });
        const array = new Int32Array(buffer);
        array[0] = i;
        buffer.resize(8);
        return array;
    },
    "typed array subarray": (log, i) => new Int32Array([9, i, 1, 2, 3]).subarray(1),
    proxy: (log, i) =>
        new Proxy([i, 1, 2, 3], {
            get(target, key, receiver) {
                log.push(`proxy get ${String(key)}`);
                return Reflect.get(target, key, receiver);
            },
            set(target, key, value, receiver) {
                log.push(`proxy set ${String(key)}`);
                return Reflect.set(target, key, value, receiver);
            },
        }),
    "array-like": (log, i) => ({ 0: i, 1: 1, length: 2 }),
    arguments: (log, i) =>
        (function () {
            return arguments;
        })(i, 1, 2, 3),
    string: (log, i) => `s${i}xyz`,
    "string object": (log, i) => new String(`s${i}`),
    null: () => null,
    undefined: () => undefined,
    number: () => 42,
};

const indices = {
    "in bounds": (log, i) => i % 4,
    first: () => 0,
    "out of bounds": () => 10,
    "far out of bounds": () => 1e9,
    negative: () => -1,
    "negative zero": () => -0,
    double: () => 1.5,
    "integral double": () => 2.0,
    NaN: () => NaN,
    "numeric string": () => "2",
    "non-numeric string": () => "length",
    "array index string": () => "4294967294",
    symbol: () => Symbol.iterator,
    object: log => ({
        toString() {
            log.push("index toString");
            return "1";
        },
    }),
    "throwing object": () => ({
        toString() {
            throw new TypeError("index toString threw");
        },
    }),
    undefined: () => undefined,
    "max int32": () => 2147483647,
    "min int32": () => -2147483648,
};

const warmUp = (log, i) => [[i, 1, 2, 3], i % 4, log, i];

const probes = {};
for (const [name, make] of Object.entries(objects))
    probes[`object: ${name}`] = (log, i) => [make(log, i), i % 4, log, i];
for (const [name, make] of Object.entries(indices))
    probes[`index: ${name}`] = (log, i) => [[i, 1, 2, 3], make(log, i), log, i];
for (const [objectName, makeObject] of Object.entries(objects)) {
    for (const indexName of ["out of bounds", "negative", "double", "numeric string"])
        probes[`object: ${objectName}, index: ${indexName}`] = (log, i) => [
            makeObject(log, i),
            indices[indexName](log, i),
            log,
            i,
        ];
}

describe("indexed gets", () => {
    const forms = {
        get: () => (object, index) => object[index],
        "get with a constant index": () => object => object[1],
        "sum in a loop": () => object => {
            let sum = 0;
            for (let k = 0; k < 4; ++k) sum = sum + object[k];
            return sum;
        },
        "get, then length": () => (object, index) => [object[index], object.length],
        "nested get": () => (object, index) => object[object[index] % 4],
        in: () => (object, index) =>
            typeof object === "object" && object !== null && typeof index !== "symbol" ? index in object : "skip",
    };
    for (const [name, factory] of Object.entries(forms)) {
        test(name, () => {
            expect(compareCompiledWithInterpreter(factory, { warmUp, probes })).toEqual([]);
        });
    }
});

describe("indexed puts", () => {
    const forms = {
        put: () => (object, index, log, i) => {
            object[index] = i;
            return [object[index], object.length];
        },
        "put of a double": () => (object, index, log, i) => {
            object[index] = i + 0.25;
            return object[index];
        },
        "put of an object": () => (object, index) => {
            object[index] = { boxed: index };
            return object[index];
        },
        append: () => (object, index, log, i) => {
            if (object === null || object === undefined) return "nullish";
            object[object.length] = i;
            return object.length;
        },
        push: () => (object, index, log, i) => {
            if (!Array.isArray(object)) return "not an array";
            return object.push(i);
        },
        increment: () => (object, index) => {
            object[index]++;
            return object[index];
        },
        "strict put": () => (object, index, log, i) => {
            "use strict";
            object[index] = i;
            return object[index];
        },
        "fill loop": () => (object, index, log, i) => {
            for (let k = 0; k < 4; ++k) object[k] = k * i;
            return object;
        },
    };
    for (const [name, factory] of Object.entries(forms)) {
        test(name, () => {
            expect(compareCompiledWithInterpreter(factory, { warmUp, probes })).toEqual([]);
        });
    }
});

describe("typed arrays warmed up by kind", () => {
    const kinds = [
        "Int8Array",
        "Uint8Array",
        "Uint8ClampedArray",
        "Int16Array",
        "Uint16Array",
        "Int32Array",
        "Uint32Array",
        "Float16Array",
        "Float32Array",
        "Float64Array",
    ];
    for (const kind of kinds) {
        test(kind, () => {
            const values = [0, 1, -1, 255, 256, -129, 65536, 2147483648, 4294967296, 0.5, -0.5, 1.5, NaN, -0, Infinity];
            const failures = compareCompiledWithInterpreter(
                () => (array, index, value) => {
                    array[index] = value;
                    return [array[index], array[index + 1]];
                },
                {
                    warmUp: (log, i) => [new globalThis[kind](4), i % 3, i],
                    probes: Object.fromEntries(
                        values.map(value => [
                            `store ${describeForComparison(value)}`,
                            () => [new globalThis[kind](4), 1, value],
                        ])
                    ),
                }
            );
            expect(failures).toEqual([]);
        });
    }
});
