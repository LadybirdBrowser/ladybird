// Compiled code runs the slow paths of instructions and generic calls on SSA values: the operands are inputs, the
// outputs are values, and the frame only gets what it needs where the slow path does not continue.

test("slow paths with several outputs", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return values => {
                let sum = 0;
                for (const value of values) sum += value;
                const [first, second] = values;
                return [sum, first, second];
            };
        },
        {
            warmUp: (log, i) => [[i, 1, 2]],
            probes: { double: () => [[0.5, 1]], string: () => [["s", 1]], empty: () => [[]] },
        }
    );
    expect(failures).toEqual([]);
});

test("slow paths that read and write their output", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return (a, b) => `${a}-${b}-${a + b}`;
        },
        { warmUp: (log, i) => [i, i + 1], probes: { double: () => [0.5, 1], string: () => ["x", "y"] } }
    );
    expect(failures).toEqual([]);
});

test("a call that throws leaves its destination's old value for the handler", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function maybeThrow(value) {
                if (value > 100) throw new RangeError(`too big: ${value}`);
                return value * 2;
            }
            return value => {
                let result = 1;
                try {
                    result = maybeThrow(value);
                } catch (e) {
                    return [result, e.message];
                }
                return [result];
            };
        },
        { warmUp: (log, i) => [i], probes: { throws: () => [200], double: () => [0.5] } }
    );
    expect(failures).toEqual([]);
});

test("calls with many operands and constant arguments, also in inlined callees", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function wide(a, b, c, d, e, f) {
                return [a, b, c, d, e, f].join(",");
            }
            function make(message) {
                return new TypeError(message + " with a constant");
            }
            function inlined(value) {
                return [
                    wide(value, 1, "two", 3, value, undefined),
                    make("made").message,
                    new RangeError("constant").message,
                ];
            }
            return value => inlined(value);
        },
        { warmUp: (log, i) => [i], probes: { double: () => [0.5], string: () => ["s"] } }
    );
    expect(failures).toEqual([]);
});
