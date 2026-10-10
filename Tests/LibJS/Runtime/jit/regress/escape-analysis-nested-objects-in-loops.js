// An object that does not escape can hold another one that does not escape. Where a loop merges the property that
// holds the inner object, the inner object must be allocated, since frame states in the loop can only describe the
// outer object with the values its properties have there. Exits in and after the
// loop rebuild the outer object, holding the inner one.

test("an object holding another one across a loop", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return values => {
                const outer = { inner: { count: 1 } };
                let sum = 0;
                for (const value of values) sum += value;
                if (typeof sum !== "number") return [sum, outer.inner.count];
                return [sum];
            };
        },
        {
            warmUp: (log, i) => [[i, 1]],
            probes: { double: () => [[0.5, 1]], string: () => [["s", 1]], empty: () => [[]] },
        }
    );
    expect(failures).toEqual([]);
});

test("an object holding another one written in a loop", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            return values => {
                const outer = { inner: { count: 0 } };
                let sum = 0;
                for (const value of values) {
                    if (value > 100) outer.inner = { count: value };
                    sum += value;
                }
                if (typeof sum !== "number") return [sum, outer.inner.count];
                return [sum];
            };
        },
        {
            warmUp: (log, i) => [[i, 1, i + 2]],
            probes: { double: () => [[0.5, 2.5]], string: () => [["s", 300]], big: () => [[200, 1]] },
        }
    );
    expect(failures).toEqual([]);
});
