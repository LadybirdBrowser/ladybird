describe("dense array access", () => {
    test("dense array read", () => {
        const arr = [1, 2, 3, 4, 5];
        let sum = 0;
        for (let i = 0; i < arr.length; i++) {
            sum += arr[i];
        }
        expect(sum).toBe(15);
    });

    test("dense array write", () => {
        const arr = [0, 0, 0, 0, 0];
        for (let i = 0; i < arr.length; i++) {
            arr[i] = i * 2;
        }
        expect(arr).toEqual([0, 2, 4, 6, 8]);
    });

    test("dense array read/write loop", () => {
        const arr = [1, 2, 3, 4, 5];
        for (let i = 0; i < arr.length; i++) {
            arr[i] = arr[i] * 2;
        }
        expect(arr).toEqual([2, 4, 6, 8, 10]);
    });

    test("dense array with 1000 elements", () => {
        const arr = [];
        for (let i = 0; i < 1000; i++) arr.push(i);
        let sum = 0;
        for (let i = 0; i < arr.length; i++) {
            sum += arr[i];
        }
        expect(sum).toBe(499500);
    });
});

describe("sparse arrays and holes", () => {
    test("array with single hole", () => {
        const arr = [1, , 3];
        expect(arr[0]).toBe(1);
        expect(arr[1]).toBeUndefined();
        expect(arr[2]).toBe(3);
        expect(1 in arr).toBeFalse();
    });

    test("array with multiple holes", () => {
        const arr = [1, , , , 5];
        expect(arr[0]).toBe(1);
        expect(arr[1]).toBeUndefined();
        expect(arr[2]).toBeUndefined();
        expect(arr[3]).toBeUndefined();
        expect(arr[4]).toBe(5);
        expect(arr.length).toBe(5);
    });

    test("sparse array with very large index", () => {
        const arr = [];
        arr[0] = 1;
        arr[1000000] = 2;
        expect(arr[0]).toBe(1);
        expect(arr[1]).toBeUndefined();
        expect(arr[1000000]).toBe(2);
        expect(arr.length).toBe(1000001);
    });

    test("filling holes", () => {
        const arr = [1, , 3];
        arr[1] = 2;
        expect(arr).toEqual([1, 2, 3]);
        expect(1 in arr).toBeTrue();
    });

    test("deleting creates holes", () => {
        const arr = [1, 2, 3];
        delete arr[1];
        expect(arr[1]).toBeUndefined();
        expect(1 in arr).toBeFalse();
        expect(arr.length).toBe(3);
    });

    test("holes vs undefined values", () => {
        const arr1 = [1, , 3];
        const arr2 = [1, undefined, 3];

        expect(arr1[1]).toBeUndefined();
        expect(arr2[1]).toBeUndefined();
        expect(1 in arr1).toBeFalse();
        expect(1 in arr2).toBeTrue();
    });

    test("prototype lookup through holes", () => {
        const proto = [, "proto"];
        const arr = Object.create(proto);
        arr[0] = "own";
        arr.length = 3;

        expect(arr[0]).toBe("own");
        expect(arr[1]).toBe("proto");
        expect(arr[2]).toBeUndefined();
    });

    test("Array.prototype pollution through holes", () => {
        const oldProto = Array.prototype[1];
        Array.prototype[1] = "polluted";

        const arr = [1, , 3];
        expect(arr[1]).toBe("polluted");

        delete Array.prototype[1];
        expect(arr[1]).toBeUndefined();

        if (oldProto !== undefined) Array.prototype[1] = oldProto;
    });
});

describe("out of bounds access", () => {
    test("read past end of array", () => {
        const arr = [1, 2, 3];
        expect(arr[3]).toBeUndefined();
        expect(arr[100]).toBeUndefined();
        expect(arr[1000000]).toBeUndefined();
    });

    test("write past end extends array", () => {
        const arr = [1, 2, 3];
        arr[5] = 6;
        expect(arr.length).toBe(6);
        expect(arr[3]).toBeUndefined();
        expect(arr[4]).toBeUndefined();
        expect(arr[5]).toBe(6);
    });

    test("negative index", () => {
        const arr = [1, 2, 3];
        expect(arr[-1]).toBeUndefined();
        arr[-1] = "negative";
        expect(arr[-1]).toBe("negative");
        expect(arr.length).toBe(3);
    });

    test("very large index", () => {
        const arr = [];
        const largeIndex = 2 ** 32 - 2;
        arr[largeIndex] = "max";
        expect(arr[largeIndex]).toBe("max");
        expect(arr.length).toBe(2 ** 32 - 1);
    });

    test("index beyond max array length", () => {
        const arr = [];
        const beyondMax = 2 ** 32;
        arr[beyondMax] = "beyond";
        expect(arr[beyondMax]).toBe("beyond");
        expect(arr.length).toBe(0);
    });
});

describe("index type coercion", () => {
    test("float index truncation", () => {
        const arr = [1, 2, 3];
        expect(arr[1.0]).toBe(2);
        expect(arr[1.5]).toBeUndefined();
        expect(arr[1.9]).toBeUndefined();
        expect(arr[0.0]).toBe(1);
    });

    test("string numeric index", () => {
        const arr = [1, 2, 3];
        expect(arr["0"]).toBe(1);
        expect(arr["1"]).toBe(2);
        expect(arr["2"]).toBe(3);
    });

    test("string non-numeric index", () => {
        const arr = [1, 2, 3];
        arr["foo"] = "bar";
        expect(arr["foo"]).toBe("bar");
        expect(arr.foo).toBe("bar");
        expect(arr.length).toBe(3);
    });

    test("index coercion with valueOf/toString", () => {
        const arr = [1, 2, 3];
        const idx = {
            toString() {
                return "1";
            },
            valueOf() {
                return 0;
            },
        };
        expect(arr[idx]).toBe(2);
    });

    test("NaN as index", () => {
        const arr = [1, 2, 3];
        expect(arr[NaN]).toBeUndefined();
        arr[NaN] = "nan";
        expect(arr["NaN"]).toBe("nan");
    });

    test("Infinity as index", () => {
        const arr = [1, 2, 3];
        expect(arr[Infinity]).toBeUndefined();
        arr[Infinity] = "inf";
        expect(arr["Infinity"]).toBe("inf");
    });

    test("-0 as index", () => {
        const arr = [1, 2, 3];
        expect(arr[-0]).toBe(1);
        expect(arr["-0"]).toBeUndefined();
    });

    test("Symbol as index", () => {
        const arr = [1, 2, 3];
        const sym = Symbol("idx");
        arr[sym] = "symbol";
        expect(arr[sym]).toBe("symbol");
        expect(arr.length).toBe(3);
    });
});

describe("object with numeric properties", () => {
    test("object with numeric string keys", () => {
        const obj = { 0: "a", 1: "b", 2: "c" };
        expect(obj[0]).toBe("a");
        expect(obj[1]).toBe("b");
        expect(obj[2]).toBe("c");
    });

    test("object with integer keys", () => {
        const obj = {};
        obj[0] = "zero";
        obj[1] = "one";
        obj[2] = "two";
        expect(obj["0"]).toBe("zero");
        expect(obj["1"]).toBe("one");
        expect(obj["2"]).toBe("two");
    });

    test("object numeric key ordering", () => {
        const obj = {};
        obj[2] = "c";
        obj[0] = "a";
        obj[1] = "b";
        const keys = Object.keys(obj);
        expect(keys).toEqual(["0", "1", "2"]);
    });

    test("array-like object", () => {
        const arrayLike = { 0: "a", 1: "b", 2: "c", length: 3 };
        expect(arrayLike[0]).toBe("a");
        expect(arrayLike[1]).toBe("b");
        expect(arrayLike[2]).toBe("c");
        expect(Array.from(arrayLike).join("")).toBe("abc");
    });

    test("arguments object indexing", () => {
        function test() {
            let sum = 0;
            for (let i = 0; i < arguments.length; i++) {
                sum += arguments[i];
            }
            return sum;
        }
        expect(test(1, 2, 3, 4, 5)).toBe(15);
    });
});

describe("TypedArray access", () => {
    test("Int8Array read/write", () => {
        const arr = new Int8Array([1, 2, 3, 4, 5]);
        let sum = 0;
        for (let i = 0; i < arr.length; i++) {
            sum += arr[i];
        }
        expect(sum).toBe(15);

        arr[0] = -128;
        arr[1] = 127;
        expect(arr[0]).toBe(-128);
        expect(arr[1]).toBe(127);
    });

    test("Int8Array overflow", () => {
        const arr = new Int8Array(1);
        arr[0] = 128;
        expect(arr[0]).toBe(-128);
        arr[0] = 256;
        expect(arr[0]).toBe(0);
    });

    test("Uint8Array read/write", () => {
        const arr = new Uint8Array([0, 127, 255]);
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(127);
        expect(arr[2]).toBe(255);

        arr[0] = 256;
        expect(arr[0]).toBe(0);
    });

    test("Uint8ClampedArray clamping", () => {
        const arr = new Uint8ClampedArray(3);
        arr[0] = -50;
        arr[1] = 300;
        arr[2] = 128;
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(255);
        expect(arr[2]).toBe(128);
    });

    test("Int16Array read/write", () => {
        const arr = new Int16Array([0, 32767, -32768]);
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(32767);
        expect(arr[2]).toBe(-32768);
    });

    test("Uint16Array read/write", () => {
        const arr = new Uint16Array([0, 32767, 65535]);
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(32767);
        expect(arr[2]).toBe(65535);
    });

    test("Int32Array read/write", () => {
        const arr = new Int32Array([0, 2147483647, -2147483648]);
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(2147483647);
        expect(arr[2]).toBe(-2147483648);
    });

    test("Uint32Array read/write", () => {
        const arr = new Uint32Array([0, 2147483647, 4294967295]);
        expect(arr[0]).toBe(0);
        expect(arr[1]).toBe(2147483647);
        expect(arr[2]).toBe(4294967295);
    });

    test("Float32Array read/write", () => {
        const arr = new Float32Array([1.5, -2.5, 3.14159]);
        expect(Math.abs(arr[0] - 1.5) < 0.0001).toBeTrue();
        expect(Math.abs(arr[1] - -2.5) < 0.0001).toBeTrue();
        expect(Math.abs(arr[2] - 3.14159) < 0.0001).toBeTrue();
    });

    test("Float64Array read/write", () => {
        const arr = new Float64Array([1.5, -2.5, Math.PI]);
        expect(arr[0]).toBe(1.5);
        expect(arr[1]).toBe(-2.5);
        expect(arr[2]).toBe(Math.PI);
    });

    test("Float64Array special values", () => {
        const arr = new Float64Array(4);
        arr[0] = NaN;
        arr[1] = Infinity;
        arr[2] = -Infinity;
        arr[3] = -0;

        expect(Number.isNaN(arr[0])).toBeTrue();
        expect(arr[1]).toBe(Infinity);
        expect(arr[2]).toBe(-Infinity);
        expect(Object.is(arr[3], -0)).toBeTrue();
    });

    test("BigInt64Array read/write", () => {
        const arr = new BigInt64Array([0n, 9007199254740991n, -9007199254740991n]);
        expect(arr[0]).toBe(0n);
        expect(arr[1]).toBe(9007199254740991n);
        expect(arr[2]).toBe(-9007199254740991n);
    });

    test("BigUint64Array read/write", () => {
        const arr = new BigUint64Array([0n, 9007199254740991n, 18446744073709551615n]);
        expect(arr[0]).toBe(0n);
        expect(arr[1]).toBe(9007199254740991n);
        expect(arr[2]).toBe(18446744073709551615n);
    });

    test("TypedArray out of bounds read", () => {
        const arr = new Int32Array([1, 2, 3]);
        expect(arr[3]).toBeUndefined();
        expect(arr[-1]).toBeUndefined();
        expect(arr[1000]).toBeUndefined();
    });

    test("TypedArray out of bounds write is ignored", () => {
        const arr = new Int32Array([1, 2, 3]);
        arr[3] = 100;
        arr[-1] = 100;
        expect(arr[3]).toBeUndefined();
        expect(arr[-1]).toBeUndefined();
        expect(arr.length).toBe(3);
    });

    test("TypedArray with shared buffer", () => {
        const buffer = new ArrayBuffer(16);
        const int32 = new Int32Array(buffer);
        const uint8 = new Uint8Array(buffer);

        int32[0] = 0x12345678;
        expect(uint8[0]).toBe(0x78);
        expect(uint8[1]).toBe(0x56);
        expect(uint8[2]).toBe(0x34);
        expect(uint8[3]).toBe(0x12);
    });

    test("TypedArray subarray access", () => {
        const arr = new Int32Array([1, 2, 3, 4, 5]);
        const sub = arr.subarray(1, 4);
        expect(sub[0]).toBe(2);
        expect(sub[1]).toBe(3);
        expect(sub[2]).toBe(4);
        expect(sub.length).toBe(3);

        sub[0] = 100;
        expect(arr[1]).toBe(100);
    });

    test("TypedArray intensive loop", () => {
        const arr = new Float64Array(10000);
        for (let i = 0; i < arr.length; i++) {
            arr[i] = i * 0.5;
        }
        let sum = 0;
        for (let i = 0; i < arr.length; i++) {
            sum += arr[i];
        }
        expect(sum).toBe(24997500);
    });
});

describe("mixed type access patterns", () => {
    test("alternating array and object access", () => {
        function getValue(obj, idx) {
            return obj[idx];
        }

        const arr = [10, 20, 30];
        const obj = { 0: 100, 1: 200, 2: 300 };

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(arr, i % 3);
            sum += getValue(obj, i % 3);
        }
        expect(sum).toBe(21890);
    });

    test("alternating TypedArray types", () => {
        function getValue(arr, idx) {
            return arr[idx];
        }

        const int32 = new Int32Array([1, 2, 3]);
        const float64 = new Float64Array([1.5, 2.5, 3.5]);
        const uint8 = new Uint8Array([10, 20, 30]);

        let sum = 0;
        for (let i = 0; i < 100; i++) {
            sum += getValue(int32, i % 3);
            sum += getValue(float64, i % 3);
            sum += getValue(uint8, i % 3);
        }
        expect(Math.abs(sum - 2438) < 0.001).toBeTrue();
    });

    test("array then TypedArray then object", () => {
        function getAndSet(container, idx, val) {
            const old = container[idx];
            container[idx] = val;
            return old;
        }

        const arr = [1, 2, 3];
        const typed = new Int32Array([10, 20, 30]);
        const obj = { 0: 100, 1: 200, 2: 300 };

        expect(getAndSet(arr, 0, 5)).toBe(1);
        expect(getAndSet(typed, 0, 50)).toBe(10);
        expect(getAndSet(obj, 0, 500)).toBe(100);

        expect(arr[0]).toBe(5);
        expect(typed[0]).toBe(50);
        expect(obj[0]).toBe(500);
    });
});

describe("accessor properties", () => {
    test("array with accessor at index", () => {
        const arr = [1, 2, 3];
        let accessCount = 0;
        Object.defineProperty(arr, 1, {
            get() {
                accessCount++;
                return 100;
            },
            set(v) {
                accessCount++;
            },
        });

        expect(arr[1]).toBe(100);
        expect(accessCount).toBe(1);
        arr[1] = 200;
        expect(accessCount).toBe(2);
    });

    test("object with indexed accessor", () => {
        const obj = {};
        let value = 42;
        Object.defineProperty(obj, 0, {
            get() {
                return value * 2;
            },
            set(v) {
                value = v;
            },
        });

        expect(obj[0]).toBe(84);
        obj[0] = 10;
        expect(obj[0]).toBe(20);
    });

    test("accessor in prototype affects indexed access", () => {
        const proto = {};
        Object.defineProperty(proto, 1, {
            get() {
                return "proto getter";
            },
        });

        const arr = Object.create(proto);
        arr[0] = "own";
        arr.length = 3;

        expect(arr[0]).toBe("own");
        expect(arr[1]).toBe("proto getter");
    });
});

describe("frozen, sealed, and non-extensible", () => {
    test("frozen array read", () => {
        const arr = Object.freeze([1, 2, 3]);
        expect(arr[0]).toBe(1);
        expect(arr[1]).toBe(2);
        expect(arr[2]).toBe(3);
    });

    test("frozen array write fails silently", () => {
        const arr = Object.freeze([1, 2, 3]);
        arr[0] = 100;
        expect(arr[0]).toBe(1);
    });

    test("sealed array modification", () => {
        const arr = Object.seal([1, 2, 3]);
        arr[0] = 100;
        expect(arr[0]).toBe(100);
        arr[5] = 5;
        expect(arr[5]).toBeUndefined();
        expect(arr.length).toBe(3);
    });

    test("non-extensible array", () => {
        const arr = Object.preventExtensions([1, 2, 3]);
        arr[0] = 100;
        expect(arr[0]).toBe(100);
        arr[5] = 5;
        expect(arr[5]).toBeUndefined();
    });

    test("TypedArray cannot be frozen", () => {
        expect(() => {
            Object.freeze(new Int32Array([1, 2, 3]));
        }).toThrow(TypeError);
    });
});

describe("detached ArrayBuffer", () => {
    test("detached TypedArray read", () => {
        const buffer = new ArrayBuffer(16);
        const arr = new Int32Array(buffer);
        arr[0] = 42;

        buffer.transfer();

        expect(arr[0]).toBeUndefined();
        expect(arr.length).toBe(0);
    });

    test("detached TypedArray write", () => {
        const buffer = new ArrayBuffer(16);
        const arr = new Int32Array(buffer);

        buffer.transfer();

        arr[0] = 42;
        expect(arr[0]).toBeUndefined();
    });
});

describe("array length interactions", () => {
    test("reducing length truncates array", () => {
        const arr = [1, 2, 3, 4, 5];
        arr.length = 3;
        expect(arr.length).toBe(3);
        expect(arr[2]).toBe(3);
        expect(arr[3]).toBeUndefined();
        expect(3 in arr).toBeFalse();
    });

    test("increasing length creates holes", () => {
        const arr = [1, 2, 3];
        arr.length = 5;
        expect(arr.length).toBe(5);
        expect(arr[3]).toBeUndefined();
        expect(arr[4]).toBeUndefined();
        expect(3 in arr).toBeFalse();
    });

    test("write beyond length updates length", () => {
        const arr = [1, 2, 3];
        arr[10] = 11;
        expect(arr.length).toBe(11);
    });

    test("length with accessor property", () => {
        const arr = [1, 2, 3];
        Object.defineProperty(arr, 5, {
            get() {
                return 6;
            },
        });
        expect(arr.length).toBe(6);
        expect(arr[5]).toBe(6);
    });
});

describe("proxy with indexed access", () => {
    test("proxy get trap for indices", () => {
        const target = [1, 2, 3];
        const proxy = new Proxy(target, {
            get(t, prop) {
                const idx = Number(prop);
                if (!isNaN(idx)) return t[prop] * 2;
                return t[prop];
            },
        });

        expect(proxy[0]).toBe(2);
        expect(proxy[1]).toBe(4);
        expect(proxy[2]).toBe(6);
    });

    test("proxy set trap for indices", () => {
        const target = [1, 2, 3];
        const proxy = new Proxy(target, {
            set(t, prop, value) {
                const idx = Number(prop);
                if (!isNaN(idx)) {
                    t[prop] = value * 2;
                    return true;
                }
                t[prop] = value;
                return true;
            },
        });

        proxy[0] = 5;
        expect(target[0]).toBe(10);
    });

    test("proxy on array-like object", () => {
        const target = { 0: 1, 1: 2, 2: 3, length: 3 };
        const proxy = new Proxy(target, {
            get(t, prop) {
                return t[prop];
            },
        });

        let sum = 0;
        for (let i = 0; i < proxy.length; i++) {
            sum += proxy[i];
        }
        expect(sum).toBe(6);
    });
});

describe("string indexing", () => {
    test("string character access", () => {
        const str = "hello";
        expect(str[0]).toBe("h");
        expect(str[1]).toBe("e");
        expect(str[4]).toBe("o");
    });

    test("string out of bounds", () => {
        const str = "hello";
        expect(str[5]).toBeUndefined();
        expect(str[-1]).toBeUndefined();
        expect(str[100]).toBeUndefined();
    });

    test("string is immutable via index", () => {
        let str = "hello";
        str[0] = "H";
        expect(str[0]).toBe("h");
        expect(str).toBe("hello");
    });

    test("String object indexing", () => {
        const str = new String("hello");
        expect(str[0]).toBe("h");
        expect(str[4]).toBe("o");
    });
});

describe("index computation edge cases", () => {
    test("index from expression", () => {
        const arr = [10, 20, 30, 40, 50];
        let sum = 0;
        for (let i = 0; i < 5; i++) {
            sum += arr[i * 1];
            sum += arr[i + 0];
            sum += arr[i | 0];
        }
        expect(sum).toBe(450);
    });

    test("index from function call", () => {
        const arr = [1, 2, 3];
        let callCount = 0;
        function getIndex() {
            return callCount++;
        }

        expect(arr[getIndex()]).toBe(1);
        expect(arr[getIndex()]).toBe(2);
        expect(arr[getIndex()]).toBe(3);
        expect(callCount).toBe(3);
    });

    test("index computation with side effects", () => {
        const arr = [1, 2, 3, 4, 5];
        let i = 0;
        const results = [];

        while (i < 5) {
            results.push(arr[i++]);
        }
        expect(results).toEqual([1, 2, 3, 4, 5]);
    });

    test("index from ternary", () => {
        const arr = [10, 20, 30];
        const flag = true;
        expect(arr[flag ? 0 : 2]).toBe(10);
        expect(arr[!flag ? 0 : 2]).toBe(30);
    });
});

describe("concurrent modification", () => {
    test("push during forward iteration", () => {
        const arr = [1, 2, 3];
        const results = [];
        for (let i = 0; i < arr.length; i++) {
            results.push(arr[i]);
            if (i === 1) arr.push(4);
        }
        expect(results).toEqual([1, 2, 3, 4]);
    });

    test("pop during forward iteration", () => {
        const arr = [1, 2, 3, 4, 5];
        const results = [];
        const len = arr.length;
        for (let i = 0; i < len; i++) {
            results.push(arr[i]);
            if (i === 2) arr.pop();
        }
        expect(results[4]).toBeUndefined();
    });

    test("shift during iteration", () => {
        const arr = [1, 2, 3, 4, 5];
        const results = [];
        for (let i = 0; i < arr.length; i++) {
            results.push(arr[i]);
            if (i === 0) arr.shift();
        }
        expect(results).toEqual([1, 3, 4, 5]);
    });

    test("splice during iteration", () => {
        const arr = [1, 2, 3, 4, 5];
        const results = [];
        for (let i = 0; i < arr.length; i++) {
            results.push(arr[i]);
            if (arr[i] === 3) arr.splice(i, 1);
        }
        expect(results).toEqual([1, 2, 3, 5]);
    });
});

describe("integer overflow edge cases", () => {
    test("index near MAX_SAFE_INTEGER", () => {
        const obj = {};
        const idx = Number.MAX_SAFE_INTEGER - 1;
        obj[idx] = "value";
        expect(obj[idx]).toBe("value");
    });

    test("index as very large number", () => {
        const obj = {};
        obj[1e20] = "large";
        expect(obj["100000000000000000000"]).toBe("large");
    });

    test("index arithmetic overflow", () => {
        const arr = [1, 2, 3];
        const bigIdx = 2 ** 53;
        expect(arr[bigIdx]).toBeUndefined();
    });
});

describe("performance-critical patterns", () => {
    test("matrix-like access pattern", () => {
        const matrix = [];
        for (let i = 0; i < 10; i++) {
            matrix[i] = [];
            for (let j = 0; j < 10; j++) {
                matrix[i][j] = i * 10 + j;
            }
        }

        let sum = 0;
        for (let i = 0; i < 10; i++) {
            for (let j = 0; j < 10; j++) {
                sum += matrix[i][j];
            }
        }
        expect(sum).toBe(4950);
    });

    test("strided access pattern", () => {
        const arr = new Float64Array(1000);
        for (let i = 0; i < 1000; i++) arr[i] = i;

        let sum = 0;
        for (let i = 0; i < 1000; i += 3) {
            sum += arr[i];
        }
        expect(sum).toBe(166833);
    });

    test("reverse iteration", () => {
        const arr = [1, 2, 3, 4, 5];
        const results = [];
        for (let i = arr.length - 1; i >= 0; i--) {
            results.push(arr[i]);
        }
        expect(results).toEqual([5, 4, 3, 2, 1]);
    });

    test("binary search pattern", () => {
        const arr = [1, 3, 5, 7, 9, 11, 13, 15, 17, 19];

        function binarySearch(arr, target) {
            let left = 0;
            let right = arr.length - 1;
            while (left <= right) {
                const mid = (left + right) >>> 1;
                if (arr[mid] === target) return mid;
                if (arr[mid] < target) left = mid + 1;
                else right = mid - 1;
            }
            return -1;
        }

        expect(binarySearch(arr, 7)).toBe(3);
        expect(binarySearch(arr, 1)).toBe(0);
        expect(binarySearch(arr, 19)).toBe(9);
        expect(binarySearch(arr, 6)).toBe(-1);
    });
});

describe("array methods that use indexed access", () => {
    test("map uses indexed access", () => {
        const arr = [1, 2, 3, 4, 5];
        const result = arr.map(x => x * 2);
        expect(result).toEqual([2, 4, 6, 8, 10]);
    });

    test("filter uses indexed access", () => {
        const arr = [1, 2, 3, 4, 5];
        const result = arr.filter(x => x % 2 === 0);
        expect(result).toEqual([2, 4]);
    });

    test("reduce uses indexed access", () => {
        const arr = [1, 2, 3, 4, 5];
        const sum = arr.reduce((a, b) => a + b, 0);
        expect(sum).toBe(15);
    });

    test("find uses indexed access", () => {
        const arr = [1, 2, 3, 4, 5];
        const found = arr.find(x => x > 3);
        expect(found).toBe(4);
    });

    test("some/every use indexed access", () => {
        const arr = [1, 2, 3, 4, 5];
        expect(arr.some(x => x > 3)).toBeTrue();
        expect(arr.every(x => x > 0)).toBeTrue();
        expect(arr.every(x => x > 3)).toBeFalse();
    });
});

describe("TypedArray specific edge cases", () => {
    test("TypedArray with byteOffset", () => {
        const buffer = new ArrayBuffer(20);
        const arr = new Int32Array(buffer, 4, 3);
        arr[0] = 10;
        arr[1] = 20;
        arr[2] = 30;

        expect(arr[0]).toBe(10);
        expect(arr[1]).toBe(20);
        expect(arr[2]).toBe(30);
        expect(arr.length).toBe(3);
    });

    test("TypedArray float to int conversion", () => {
        const arr = new Int32Array(3);
        arr[0] = 1.9;
        arr[1] = -1.9;
        arr[2] = 3.5;

        expect(arr[0]).toBe(1);
        expect(arr[1]).toBe(-1);
        expect(arr[2]).toBe(3);
    });

    test("TypedArray with NaN value", () => {
        const int = new Int32Array(1);
        int[0] = NaN;
        expect(int[0]).toBe(0);

        const float = new Float64Array(1);
        float[0] = NaN;
        expect(Number.isNaN(float[0])).toBeTrue();
    });

    test("TypedArray with Infinity", () => {
        const int = new Int32Array(1);
        int[0] = Infinity;
        expect(int[0]).toBe(0);

        const float = new Float64Array(1);
        float[0] = Infinity;
        expect(float[0]).toBe(Infinity);
    });

    test("TypedArray set from array", () => {
        const arr = new Int32Array(5);
        arr.set([1, 2, 3], 1);
        expect([...arr]).toEqual([0, 1, 2, 3, 0]);
    });

    test("TypedArray copyWithin", () => {
        const arr = new Int32Array([1, 2, 3, 4, 5]);
        arr.copyWithin(0, 3);
        expect([...arr]).toEqual([4, 5, 3, 4, 5]);
    });
});

describe("weird but valid JavaScript", () => {
    test("array with length property as non-writable", () => {
        const arr = [1, 2, 3];
        Object.defineProperty(arr, "length", { writable: false });

        expect(() => {
            arr.push(4);
        }).toThrow(TypeError);
        expect(arr.length).toBe(3);
    });

    test("array subclass", () => {
        class MyArray extends Array {
            sum() {
                let s = 0;
                for (let i = 0; i < this.length; i++) s += this[i];
                return s;
            }
        }

        const arr = new MyArray(1, 2, 3, 4, 5);
        expect(arr.sum()).toBe(15);
        expect(arr[2]).toBe(3);
    });

    test("array with getter for length", () => {
        const obj = {
            0: "a",
            1: "b",
            2: "c",
            get length() {
                return 3;
            },
        };

        expect(Array.prototype.join.call(obj, "-")).toBe("a-b-c");
    });

    test("accessing array with call/apply", () => {
        const arr = [1, 2, 3];
        const get = function (i) {
            return this[i];
        };

        expect(get.call(arr, 0)).toBe(1);
        expect(get.apply(arr, [1])).toBe(2);
    });
});
