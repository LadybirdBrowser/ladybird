// Callees with exception handlers, like the handler that closes the iterator of a for-of loop, are inlined when none
// of their handlers caught an exception before the compile. An exception that one of them catches leaves compiled code,
// and the interpreter runs the handler in the callee's frame.
//
// NB: The functions come from the Function constructor, so that each run of a test (see LIBJS_TEST_REPEAT) compiles
//     new ones, whose handlers never caught anything.

function prepared(source) {
    const functions = new Function(source)();
    for (const f of Object.values(functions)) jit.prepare(f);
    return functions;
}

function compile(f, warm_up) {
    for (let i = 0; i < 10; ++i) warm_up();
    expect(jit.compile(f)).toBe(jit.enabled);
}

function closingIterable(values, log) {
    return {
        [Symbol.iterator]() {
            let index = 0;
            return {
                next: () => (index < values.length ? { value: values[index++], done: false } : { done: true }),
                return: () => {
                    log.push("return");
                    return {};
                },
            };
        },
    };
}

function* closingGenerator(values, log) {
    try {
        yield* values;
    } finally {
        log.push("return");
    }
}

describe("for-of loops in inlined callees", () => {
    test("an exception from the loop body closes the iterator once and reaches the caller's handler", () => {
        const { run } = prepared(`
            function sum(iterable, error) {
                let total = 0;
                for (const value of iterable) {
                    if (value < 0) throw error;
                    total += value;
                }
                return total;
            }
            function run(iterable, error) {
                try {
                    return sum(iterable, error);
                } catch (e) {
                    return e;
                }
            }
            return { sum, run };
        `);
        const error = new Error("negative");
        compile(run, () => {
            run(closingIterable([1, 2], []), error);
            run(closingGenerator([1, 2], []), error);
        });
        for (const iterable of [closingIterable, closingGenerator]) {
            const log = [];
            expect(run(iterable([1, -1, 2], log), error)).toBe(error);
            expect(log).toEqual(["return"]);
        }
        expect(run([1, 2, 3], error)).toBe(6);
        expect(run([1, -2, 3], error)).toBe(error);
    });

    test("break and return close a generator once", () => {
        const { run } = prepared(`
            function firstAbove(iterable, limit) {
                for (const value of iterable) {
                    if (value > limit) return value;
                }
                return -1;
            }
            function countUpTo(iterable, limit) {
                let count = 0;
                for (const value of iterable) {
                    if (value > limit) break;
                    ++count;
                }
                return count;
            }
            function run(make, values, limit, log) {
                return [firstAbove(make(values, log), limit), countUpTo(make(values, log), limit)];
            }
            return { firstAbove, countUpTo, run };
        `);
        compile(run, () => run(closingGenerator, [1, 5, 2], 3, []));
        let log = [];
        expect(run(closingGenerator, [1, 5, 2], 3, log)).toEqual([5, 1]);
        expect(log).toEqual(["return", "return"]);
        log = [];
        expect(run(closingGenerator, [1, 2], 3, log)).toEqual([-1, 2]);
        expect(log).toEqual(["return", "return"]);
        log = [];
        expect(run(closingIterable, [7, 1], 3, log)).toEqual([7, 0]);
        expect(log).toEqual(["return", "return"]);
    });
});

describe("exception handlers in inlined callees", () => {
    test("a handler of an inlined callee gets the exception it caught", () => {
        const { run } = prepared(`
            function doubled(value) {
                try {
                    if (typeof value !== "number") throw value;
                    return value * 2;
                } catch (e) {
                    return "not a number: " + e;
                }
            }
            function run(values) {
                const results = [];
                for (let i = 0; i < values.length; ++i) results.push(doubled(values[i]));
                return results;
            }
            return { doubled, run };
        `);
        compile(run, () => run([1, 2, 3]));
        expect(run([1, "a", 3])).toEqual([2, "not a number: a", 6]);
        for (let i = 0; i < 100; ++i) expect(run([i, "b"])).toEqual([2 * i, "not a number: b"]);
    });

    test("an exception thrown in a nested inlined callee reaches the handler of the callee it is inlined into", () => {
        const { run } = prepared(`
            function checked(value) {
                if (value < 0) throw value;
                return value;
            }
            function sum(values) {
                let total = 0;
                try {
                    for (const value of values) total += checked(value);
                } catch (e) {
                    return [e, total];
                }
                return total;
            }
            function run(values) {
                return sum(values);
            }
            return { checked, sum, run };
        `);
        compile(run, () => run([1, 2, 3]));
        expect(run([1, 2, -3, 4])).toEqual([-3, 3]);
        for (let i = 0; i < 100; ++i) expect(run([i, -i - 1])).toEqual([-i - 1, i]);
        expect(run([4, 5])).toBe(9);
    });
});
