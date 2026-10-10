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
