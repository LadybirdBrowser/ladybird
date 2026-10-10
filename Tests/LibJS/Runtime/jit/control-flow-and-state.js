// Exceptions, loops, closures and globals in compiled code, warmed up with one kind of input and then given others
// that make it exit, throw or take other paths. The compiled code must do what the interpreter does (see
// compareCompiledWithInterpreter in test-common.js).

const values = {
    int32: (log, i) => i,
    double: () => 2.5,
    string: () => "7",
    object: log => ({
        valueOf() {
            log.push("valueOf");
            return 3;
        },
    }),
    throwing: () => ({
        valueOf() {
            throw new SyntaxError("valueOf threw");
        },
    }),
    undefined: () => undefined,
    null: () => null,
    bigint: () => 3n,
    "negative zero": () => -0,
    large: () => 2147483647,
    small: () => -2147483648,
};

const probes = {};
for (const [name, make] of Object.entries(values)) probes[name] = (log, i) => [make(log, i), log];

const warmUp = (log, i) => [i % 10, log];

function check(factory) {
    expect(compareCompiledWithInterpreter(factory, { warmUp, probes })).toEqual([]);
}

describe("exceptions", () => {
    test("throw and catch in the same function", () =>
        check(() => (value, log) => {
            try {
                if (value > 5) throw new Error(`big ${value}`);
                return value * 2;
            } catch (e) {
                log.push(e.message);
                return -1;
            }
        }));

    test("finally overriding the return value", () =>
        check(() => (value, log) => {
            try {
                return value + 1;
            } finally {
                log.push("finally");
                if (value === 3) return "overridden";
            }
        }));

    test("rethrow from catch", () =>
        check(() => (value, log) => {
            try {
                try {
                    return value * value;
                } catch (e) {
                    log.push("inner catch");
                    throw new TypeError(`rethrown ${e.message}`);
                }
            } catch (e) {
                return `outer caught ${e}`;
            }
        }));

    test("loop with try inside", () =>
        check(() => (value, log) => {
            let total = 0;
            for (let i = 0; i < 5; ++i) {
                try {
                    total += value * i;
                    if (i === 3) throw i;
                } catch (e) {
                    total -= e;
                }
            }
            return total;
        }));

    test("exception from a native called in a loop", () =>
        check(() => (value, log) => {
            const results = [];
            for (let i = 0; i < 3; ++i) {
                try {
                    results.push(BigInt(value) + BigInt(i));
                } catch (e) {
                    results.push(e.constructor.name);
                }
            }
            return results;
        }));
});

describe("loops", () => {
    test("int32 counter with an overflowing sum", () =>
        check(() => value => {
            let sum = 0;
            for (let i = 0; i < 10; ++i) sum += value * 1000000;
            return sum;
        }));

    test("loop whose phi changes type", () =>
        check(() => value => {
            let x = 0;
            for (let i = 0; i < 6; ++i) x = i === 3 ? value : x + 1;
            return x;
        }));

    test("nested loops with breaks and continues", () =>
        check(() => value => {
            let count = 0;
            outer: for (let i = 0; i < 5; ++i) {
                for (let j = 0; j < 5; ++j) {
                    if (j === value) continue outer;
                    if (i * j > 8) break outer;
                    count += i ^ j;
                }
            }
            return count;
        }));

    test("for-in over a changing object", () =>
        check(() => (value, log) => {
            const object = { a: 1, b: 2, c: value };
            const keys = [];
            for (const key in object) {
                keys.push(key);
                if (key === "a") delete object.b;
                if (key === "c") object.d = 4;
            }
            return keys;
        }));

    test("for-of over arrays and strings", () =>
        check(() => value => {
            const seen = [];
            for (const element of [value, value + 1]) seen.push(element);
            for (const character of `${value}`) seen.push(character);
            return seen;
        }));

    test("do-while counting down", () =>
        check(() => value => {
            let n = typeof value === "number" && value === value && Math.abs(value) < 100 ? value : 3;
            let steps = 0;
            do {
                n = n - 1;
                ++steps;
            } while (n > 0 && steps < 200);
            return [n, steps];
        }));
});

describe("closures and environments", () => {
    test("closure created in the compiled function", () =>
        check(() => value => {
            const add = x => x + value;
            return [add(1), add(value)];
        }));

    test("closures in a loop capture their own binding", () =>
        check(() => value => {
            const functions = [];
            for (let i = 0; i < 3; ++i) functions.push(() => i + value);
            return functions.map(f => f());
        }));

    test("callee changes the caller's variable", () =>
        check(() => {
            let shared = 0;
            const bump = amount => {
                shared += amount;
            };
            return value => {
                shared = value;
                bump(1);
                return shared;
            };
        }));

    test("arguments object and named parameter alias", () =>
        check(
            () =>
                function (value) {
                    arguments[0] = "replaced";
                    return [value, arguments.length];
                }
        ));

    test("eval sees the locals", () =>
        check(() => value => {
            const local = value;
            return eval("local");
        }));
});

describe("globals", () => {
    test("global variable changing type", () =>
        check(() => value => {
            globalThis.jitTestGlobal = value;
            return globalThis.jitTestGlobal;
        }));

    test("global function replaced", () =>
        check(() => (value, log) => {
            const saved = globalThis.parseInt;
            if (value === 7 || value === "7") globalThis.parseInt = () => "replaced";
            try {
                return parseInt("12", 10);
            } finally {
                globalThis.parseInt = saved;
            }
        }));
});
