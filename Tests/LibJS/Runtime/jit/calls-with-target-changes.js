// Calls, compiled (and inlined) after warming up with one callee, then given other callees: other functions, closures
// of the same function, natives, bound functions, proxies, classes, getters, callees that throw or re-enter the
// compiled code, and calls with too few or too many arguments. The compiled code must do what the interpreter does
// (see compareCompiledWithInterpreter in test-common.js).

const callees = {
    "same function": () => (a, b) => a + b,
    "other function": () => (a, b) => a * b,
    "closure with state": log => {
        let calls = 0;
        return (a, b) => {
            log.push(`closure call ${++calls}`);
            return a - b;
        };
    },
    "function using arguments": () =>
        function () {
            return arguments.length + arguments[0];
        },
    "function with rest":
        () =>
        (...rest) =>
            rest.length,
    "function with defaults":
        () =>
        (a, b = 100) =>
            a + b,
    "function returning nothing": () => () => {},
    "throwing function": log => () => {
        log.push("throwing");
        throw new EvalError("callee threw");
    },
    "throwing a primitive": () => () => {
        throw 42;
    },
    "deep recursion": () => {
        const recurse = n => (n <= 0 ? 0 : 1 + recurse(n - 1));
        return a => recurse(a % 50);
    },
    native: () => Math.max,
    "native that throws": () => Object.defineProperty,
    "bound function": () =>
        function (a, b) {
            return [this.tag, a, b];
        }.bind({ tag: "bound" }, "first"),
    proxy: log =>
        new Proxy((a, b) => a + b, {
            apply(target, thisArgument, args) {
                log.push(`apply ${args.length}`);
                return Reflect.apply(target, thisArgument, args);
            },
        }),
    class: () => class {},
    generator: () =>
        function* (a) {
            yield a;
        },
    "async function": () => async a => a,
    "not callable": () => ({}),
    undefined: () => undefined,
    "getter calling": log => {
        const object = {
            get value() {
                log.push("getter");
                return 7;
            },
        };
        return a => object.value + a;
    },
    "callee that calls back": log => (a, b, again) => {
        if (typeof again === "function" && a > 0) {
            log.push("re-entering");
            return again(again, a - 1);
        }
        return a;
    },
    "callee that collects garbage": () => (a, b) => {
        gc();
        return a + b;
    },
    "strict this": () =>
        function () {
            "use strict";
            return this;
        },
    "sloppy this": () =>
        function () {
            return typeof this;
        },
};

const probes = {};
for (const [name, make] of Object.entries(callees)) probes[name] = (log, i) => [make(log), i, log];

describe("calls of changing callees", () => {
    const forms = {
        call: () => (callee, i) => callee(i, 2),
        "call with too few arguments": () => (callee, i) => callee(i),
        "call with too many arguments": () => (callee, i) => callee(i, 2, 3, 4, 5),
        "call with spread": () => (callee, i) => callee(...[i, 2]),
        "method call": () => (callee, i) => {
            const object = { method: callee, tag: "object" };
            return object.method(i, 2);
        },
        "call in a loop": () => (callee, i) => {
            let result = [];
            for (let k = 0; k < 3; ++k) result.push(callee(i + k, k));
            return result;
        },
        "call caught": () => (callee, i) => {
            try {
                return callee(i, 2);
            } catch (e) {
                return `caught ${e}`;
            }
        },
        "call with finally": () => (callee, i, log) => {
            let state = "before";
            try {
                state = "calling";
                return [callee(i, 2), state];
            } finally {
                log.push(`finally saw ${state}`);
            }
        },
        construct: () => (callee, i) => {
            const made = new callee(i, 2);
            return typeof made;
        },
        "call through apply": () => (callee, i) => callee.apply(undefined, [i, 2]),
        "call through call": () => (callee, i) => callee.call({ tag: "this" }, i, 2),
        "re-entrant call": () => {
            const self = (callee, i, log) =>
                typeof callee === "function" ? callee(i % 3, 2, (again, a) => self(callee, a, log)) : "not callable";
            return self;
        },
    };
    for (const [name, factory] of Object.entries(forms)) {
        test(name, () => {
            const failures = compareCompiledWithInterpreter(factory, {
                warmUp: (log, i) => [(a, b) => a + b, i, log],
                probes,
            });
            expect(failures).toEqual([]);
        });
    }
});

describe("inlined calls", () => {
    test("exits at every depth of an inlined chain", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const leaf = value => value.amount * 2;
                const middle = value => leaf(value) + 1;
                const outer = value => [middle(value), value.amount];
                return value => outer(value);
            },
            {
                warmUp: (log, i) => [{ amount: i }],
                probes: {
                    double: () => [{ amount: 1.5 }],
                    string: () => [{ amount: "3" }],
                    "other shape": () => [{ other: 1, amount: 2 }],
                    getter: log => [
                        {
                            get amount() {
                                log.push("amount getter");
                                return 4;
                            },
                        },
                    ],
                    "throwing getter": () => [
                        {
                            get amount() {
                                throw new Error("amount getter threw");
                            },
                        },
                    ],
                    overflow: () => [{ amount: 2147483647 }],
                    null: () => [null],
                },
            }
        );
        expect(failures).toEqual([]);
    });

    test("locals of the caller survive exceptions in inlined callees", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const check = value => {
                    if (value > 5) throw new RangeError(`too big: ${value}`);
                    return value;
                };
                return (value, log) => {
                    let local = value * 2;
                    const other = { value };
                    try {
                        local += check(value);
                    } catch (e) {
                        log.push(`caught ${e.message}, local is ${local}, other is ${other.value}`);
                        local = -local;
                    }
                    return [local, other.value];
                };
            },
            {
                warmUp: (log, i) => [i % 5, log],
                probes: { throwing: log => [9, log], double: log => [2.5, log], string: log => ["4", log] },
            }
        );
        expect(failures).toEqual([]);
    });

    test("results of native calls in inlined callees", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const inner = (a, b) => Math.max(a, b) + Math.min(a, b) * 2;
                return (a, b) => [inner(a, b), inner(b, a)];
            },
            {
                warmUp: (log, i) => [i, 3],
                probes: {
                    doubles: () => [1.5, -0],
                    strings: () => ["7", "2"],
                    objects: () => [{ valueOf: () => 3 }, 1],
                },
            }
        );
        expect(failures).toEqual([]);
    });

    test("arguments objects of inlined callees", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                function inner() {
                    return [arguments.length, arguments[0], arguments[arguments.length - 1]];
                }
                function mapped(a) {
                    arguments[0] = "changed";
                    return a;
                }
                return (a, b) => [inner(a, b), inner(a), mapped(a)];
            },
            {
                warmUp: (log, i) => [i, 2],
                probes: { undefined: () => [undefined, undefined], object: () => [{}, []] },
            }
        );
        expect(failures).toEqual([]);
    });

    test("this of inlined methods", () => {
        const failures = compareCompiledWithInterpreter(
            () => {
                const prototype = {
                    total() {
                        return this.a + this.b;
                    },
                    scaled(factor) {
                        return this.total() * factor;
                    },
                };
                return (a, b) => {
                    const object = Object.create(prototype);
                    object.a = a;
                    object.b = b;
                    return object.scaled(2);
                };
            },
            {
                warmUp: (log, i) => [i, 1],
                probes: { strings: () => ["x", "y"], overflow: () => [2147483647, 2147483647] },
            }
        );
        expect(failures).toEqual([]);
    });
});
