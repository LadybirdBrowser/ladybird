// The legacy f.arguments property shows the arguments of the frame running f. Compiled code inlining f must show the
// arguments f was called with: read by f itself, and read after an exit rebuilt f's frame, also arguments f no longer
// needs there.

test("an inlined function reads its own legacy arguments", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function callee(a, b) {
                return [callee.arguments.length, callee.arguments[0], callee.arguments[1]];
            }
            return x => callee(x, x + 1);
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["s"] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function reads its legacy arguments after an exit", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function inspect(value) {
                const doubled = value * 2;
                return [doubled, middle.arguments.length, middle.arguments[1]];
            }
            function middle(a, b, c) {
                if (c === null) return b;
                return inspect(a + 1);
            }
            return a => middle(a, `b${a}`, { c: a });
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["7"] } }
    );
    expect(failures).toEqual([]);
});

test("an inlined function reads legacy arguments it has no parameters for", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function callee(a) {
                return [callee.arguments.length, callee.arguments[1], callee.arguments[2]];
            }
            return x => callee(x, x + 1, `z${x}`);
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["s"] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function reads its legacy arguments without parameters after an exit", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function inspect(value) {
                const doubled = value * 2;
                return [doubled, middle.arguments.length, middle.arguments[2]];
            }
            function middle(a) {
                return inspect(a + 1);
            }
            return a => middle(a, 1, `c${a}`);
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["7"] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function adds a property to an object in its legacy arguments", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function addProperty(add) {
                // A debugger statement keeps this function from being inlined.
                debugger;
                if (add) middle.arguments[0].z = 3;
            }
            function middle(object, add) {
                addProperty(add);
                object.y = 2;
            }
            return add => {
                const object = { x: 1 };
                middle(object, add);
                return object;
            };
        },
        { warmUp: () => [false], probes: { add: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function changes a property of an object in its legacy arguments", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function changeProperty(change) {
                // A debugger statement keeps this function from being inlined.
                debugger;
                if (change) middle.arguments[0].x = 2;
            }
            function middle(object, change) {
                changeProperty(change);
            }
            return change => {
                const object = { x: 1 };
                middle(object, change);
                return object.x;
            };
        },
        { warmUp: () => [false], probes: { change: () => [true] } }
    );
    expect(failures).toEqual([]);
});

test("a function called by an inlined function reads its legacy arguments after a branch", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function readArguments(read) {
                // A debugger statement keeps this function from being inlined.
                debugger;
                if (read) return [...middle.arguments];
            }
            function middle(value, flag, read) {
                if (flag) value = 1;
                else value = 2;
                return readArguments(read);
            }
            jit.neverCompile(middle);
            return (flag, read) => middle(0, flag, read);
        },
        { warmUp: (log, i) => [i % 2 === 0, false], probes: { read: () => [true, true] } }
    );
    expect(failures).toEqual([]);
});

test("functions called by the compiled function read a parameter it reassigned from its legacy arguments", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function readArgument(read) {
                // A debugger statement keeps this function from being inlined.
                debugger;
                if (read) return outer.arguments[0].x;
            }
            function middle(read) {
                return readArgument(read);
            }
            function outer(object, count, read) {
                if (count > 1) object = { x: 1 };
                else object = { x: 2 };
                const afterBranch = readArgument(read);
                for (let i = 0; i < count; ++i) object = { x: i + 3 };
                return [afterBranch, middle(read)];
            }
            return outer;
        },
        { warmUp: (log, i) => [null, i % 3, false], probes: { read: () => [null, 2, true] } }
    );
    expect(failures).toEqual([]);
});
