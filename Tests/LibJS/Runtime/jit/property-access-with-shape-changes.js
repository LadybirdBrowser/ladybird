// Named property gets and puts, compiled after warming up with objects of one shape, then given every other kind of
// receiver: other shapes, prototype properties, accessors, proxies, frozen and dictionary objects, primitives. The
// compiled code must do what the interpreter does (see compareCompiledWithInterpreter in test-common.js).

// Receivers for gets and puts of the property `x`, by name. Each makes a fresh receiver with the call's log.
const receivers = {
    "same shape": (log, i) => ({ x: i, y: 2 }),
    "other order": (log, i) => ({ y: 2, x: i }),
    "more properties": (log, i) => ({ x: i, y: 2, z: 3, w: 4 }),
    "missing property": (log, i) => ({ y: i }),
    "on the prototype": (log, i) => Object.create({ x: i }),
    "deep on the prototype": (log, i) => Object.create(Object.create(Object.create({ x: i }))),
    "own getter": (log, i) => ({
        get x() {
            log.push("get");
            return i * 10;
        },
        set x(value) {
            log.push(`set ${value}`);
        },
    }),
    "getter on the prototype": (log, i) =>
        Object.create({
            get x() {
                log.push("proto get");
                return this.y;
            },
            set x(value) {
                log.push(`proto set ${value}`);
            },
            y: i,
        }),
    "throwing getter": log => ({
        get x() {
            log.push("throw");
            throw new RangeError("getter threw");
        },
        set x(value) {
            throw new RangeError(`setter threw ${value}`);
        },
    }),
    proxy: (log, i) =>
        new Proxy(
            { x: i },
            {
                get(target, key, receiver) {
                    log.push(`proxy get ${String(key)}`);
                    return Reflect.get(target, key, receiver);
                },
                set(target, key, value, receiver) {
                    log.push(`proxy set ${String(key)}`);
                    return Reflect.set(target, key, value, receiver);
                },
            }
        ),
    frozen: (log, i) => Object.freeze({ x: i, y: 2 }),
    sealed: (log, i) => Object.seal({ y: 2 }),
    "non-extensible": (log, i) => Object.preventExtensions({ y: i }),
    "non-writable": (log, i) => Object.defineProperty({ y: 2 }, "x", { value: i, writable: false }),
    "non-writable on the prototype": (log, i) =>
        Object.create(Object.defineProperty({}, "x", { value: i, writable: false })),
    dictionary: (log, i) => {
        const object = { a: 1, b: 2, x: i };
        for (let k = 0; k < 40; ++k) object[`p${k}`] = k;
        delete object.a;
        return object;
    },
    "deleted property": (log, i) => {
        const object = { x: i, y: 2 };
        delete object.x;
        return object;
    },
    "null prototype": (log, i) => Object.assign(Object.create(null), { x: i }),
    array: (log, i) => [i, 2, 3],
    function: () => function x() {},
    string: (log, i) => `string ${i}`,
    number: (log, i) => i + 0.5,
    boolean: () => true,
    symbol: () => Symbol("receiver"),
    bigint: () => 5n,
    undefined: () => undefined,
    null: () => null,
    "class instance": (log, i) =>
        new (class {
            constructor() {
                this.x = i;
            }
        })(),
    "private-looking": (log, i) => ({ ["x"]: i, ["#x"]: 1 }),
};

const probes = {};
for (const [name, make] of Object.entries(receivers)) probes[name] = (log, i) => [make(log, i), log, i];

const warmUp = (log, i) => [{ x: i, y: 2 }, log, i];

describe("named property gets", () => {
    const forms = {
        get: () => object => object.x,
        "get and add": () => object => object.x + object.y,
        "get in a loop": () => object => {
            let sum = 0;
            for (let i = 0; i < 3; ++i) sum += object.x;
            return sum;
        },
        "get of a method call": () => object => {
            const value = object.x;
            return typeof value === "function" ? value.call(object) : value;
        },
        "optional chaining": () => object => object?.x,
        "get through a helper": () => {
            const read = object => object.x;
            return object => read(object) ?? "nothing";
        },
        in: () => object => (typeof object === "object" && object !== null ? "x" in object : "primitive"),
        hasOwnProperty: () => object =>
            object === null || object === undefined ? "nullish" : Object.prototype.hasOwnProperty.call(object, "x"),
    };
    for (const [name, factory] of Object.entries(forms)) {
        test(name, () => {
            expect(compareCompiledWithInterpreter(factory, { warmUp, probes })).toEqual([]);
        });
    }
});

describe("named property puts", () => {
    const forms = {
        put: () => (object, log, i) => {
            object.x = i;
            return object.x;
        },
        "strict put": () => (object, log, i) => {
            "use strict";
            object.x = i;
            return object.x;
        },
        "put of a new property": () => (object, log, i) => {
            object.added = i;
            return [object.added, object.x];
        },
        increment: () => object => {
            object.x++;
            return object.x;
        },
        "compound assignment": () => (object, log, i) => {
            object.x += i;
            return object.x;
        },
        delete: () => object => {
            const deleted = delete object.x;
            return [deleted, object.x];
        },
    };
    for (const [name, factory] of Object.entries(forms)) {
        test(name, () => {
            expect(compareCompiledWithInterpreter(factory, { warmUp, probes })).toEqual([]);
        });
    }
});

describe("polymorphic receivers", () => {
    test("gets warmed up with two shapes", () => {
        const failures = compareCompiledWithInterpreter(() => object => object.x, {
            warmUp: (log, i) => [i % 2 ? { x: i } : { y: 1, x: i }],
            probes,
        });
        expect(failures).toEqual([]);
    });

    test("gets warmed up with five shapes", () => {
        const failures = compareCompiledWithInterpreter(() => object => object.x, {
            warmUp: (log, i) => {
                const object = { x: i };
                object[`p${i % 5}`] = 1;
                return [object];
            },
            probes,
        });
        expect(failures).toEqual([]);
    });
});

describe("prototype changes after compilation", () => {
    test("a prototype gains a getter", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const prototype = { x: 1 };
                return (mutate, log) => {
                    if (mutate === 1)
                        Object.defineProperty(prototype, "x", {
                            get() {
                                log.push("getter");
                                return 2;
                            },
                            configurable: true,
                        });
                    if (mutate === 2) Object.defineProperty(prototype, "x", { value: 3, configurable: true });
                    return Object.create(prototype).x;
                };
            },
            {
                warmUp: log => [0, log],
                probes: { getter: log => [1, log], "data again": log => [2, log] },
            }
        );
        expect(failures).toEqual([]);
    });

    test("an object's prototype is replaced", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const object = Object.create({ x: 1, method: () => "first" });
                return swap => {
                    if (swap) Object.setPrototypeOf(object, { x: 2, method: () => "second" });
                    return [object.x, object.method()];
                };
            },
            { warmUp: () => [false], probes: { swapped: () => [true] } }
        );
        expect(failures).toEqual([]);
    });

    test("a property shadows the prototype's", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const prototype = { x: 1 };
                const object = Object.create(prototype);
                return shadow => {
                    if (shadow) object.x = 5;
                    return object.x;
                };
            },
            { warmUp: () => [false], probes: { shadowed: () => [true] } }
        );
        expect(failures).toEqual([]);
    });

    test("Object.prototype gains the property", () => {
        const failures = compareCompiledWithInterpreter(
            () => (add, log) => {
                const object = { y: 1 };
                if (add) Object.prototype.jitTestProperty = "from Object.prototype";
                try {
                    return object.jitTestProperty;
                } finally {
                    delete Object.prototype.jitTestProperty;
                }
            },
            { warmUp: log => [false, log], probes: { added: log => [true, log] } }
        );
        expect(failures).toEqual([]);
    });
});
