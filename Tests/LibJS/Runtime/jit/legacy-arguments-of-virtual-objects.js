// Code that inlined functions call can read their arguments through the legacy f.arguments property, also objects the
// caller made just for the call. Compiled code must show the very objects the functions have: reading them twice gives
// the same object, and writes through f.arguments show in the functions.

test("a function called by an inlined function reads and writes an object it never allocated", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function access(accessing) {
                // A debugger statement keeps this function from being inlined.
                debugger;
                if (!accessing) return 0;
                const seen = middle.arguments[0].x;
                middle.arguments[0].x = 9;
                return seen;
            }
            function middle(object, accessing) {
                object.x = 2;
                const seen = access(accessing);
                return [seen, object.x];
            }
            return accessing => middle({ x: 1 }, accessing);
        },
        { warmUp: () => [false], probes: { access: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a function run by the interpreter reads and writes an object an inlined function never allocated", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function access(accessing) {
                if (!accessing) return 0;
                const seen = middle.arguments[0].x;
                middle.arguments[0].x = 9;
                return seen;
            }
            jit.neverCompile(access);
            function middle(object, accessing) {
                object.x = 2;
                const seen = access(accessing);
                return [seen, object.x];
            }
            return accessing => middle({ x: 1 }, accessing);
        },
        { warmUp: () => [false], probes: { access: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("properties added through f.arguments show in the inlined function", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function add(adding) {
                debugger;
                if (adding) middle.arguments[0].added = 6;
            }
            function middle(object, adding) {
                add(adding);
                return [object.x, object.added, Object.keys(object).length];
            }
            return adding => middle({ x: 1 }, adding);
        },
        { warmUp: () => [false], probes: { add: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("reading f.arguments twice in one call gives the same object", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function read(reading) {
                debugger;
                if (reading) return middle.arguments[0] === middle.arguments[0];
            }
            function middle(object, reading) {
                return [read(reading), object.x];
            }
            return reading => middle({ x: 1 }, reading);
        },
        { warmUp: () => [false], probes: { read: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("objects in objects read through f.arguments are the ones the function has", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function write(writing) {
                debugger;
                if (writing) middle.arguments[0].inner.y = 5;
            }
            function middle(object, writing) {
                const inner = object.inner;
                write(writing);
                inner.y += 1;
                return [inner.y, object.inner.y];
            }
            return writing => middle({ inner: { y: 1 } }, writing);
        },
        { warmUp: () => [false], probes: { write: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("an object read through f.arguments stays the function's object after the read", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const stash = [];
            function read(reading) {
                debugger;
                if (reading) stash.push(middle.arguments[0]);
            }
            function middle(object, reading) {
                read(reading);
                object.x += 10;
                return object.x;
            }
            return reading => {
                stash.length = 0;
                const x = middle({ x: 1 }, reading);
                return [x, stash.length, stash.length ? stash[0].x : "none"];
            };
        },
        { warmUp: () => [false], probes: { read: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a slow path in an inlined function runs code that reads an object the caller never allocated", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function middle(object, other) {
                const number = +other;
                return [number, object.x];
            }
            return reading =>
                middle(
                    { x: 1 },
                    {
                        valueOf() {
                            if (!reading) return 2;
                            const seen = middle.arguments[0];
                            seen.x = 7;
                            return seen.x;
                        },
                    }
                );
        },
        { warmUp: () => [false], probes: { read: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a for-in loop in a function inlined into an inlined function runs a trap that reads its caller's object", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function keys(target) {
                const result = [];
                for (const key in target) result.push(key);
                return result;
            }
            function middle(object, target) {
                const found = keys(target);
                return [found, object.x];
            }
            return reading => {
                const handler = {
                    ownKeys(target) {
                        if (reading) middle.arguments[0].x = 3;
                        return Reflect.ownKeys(target);
                    },
                };
                return middle({ x: 1 }, new Proxy({ a: 1 }, handler));
            };
        },
        { warmUp: () => [false], probes: { read: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function reads an object it never allocated and throws", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            let stashed;
            function read(reading) {
                debugger;
                if (reading) {
                    stashed = middle.arguments[0];
                    throw new Error("read");
                }
            }
            function middle(object, reading) {
                read(reading);
                return object.x;
            }
            return reading => {
                stashed = undefined;
                const object = { x: 1 };
                try {
                    return middle({ x: 1, object }, reading);
                } catch (e) {
                    return [e.message, stashed.x, stashed.object === object];
                }
            };
        },
        { warmUp: () => [false], probes: { read: () => [true] } }
    );
    expect(failures).toEqual([]);
});
