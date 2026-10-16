// Generators run in compiled code: a call starts one there, and each resumption enters the code at the continuation
// of the yield it was suspended at, with the values that the frame kept across the suspension.
//
// NB: The functions come from the GeneratorFunction constructor, so that each run of a test (see LIBJS_TEST_REPEAT)
//     compiles new ones.

const GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor;

function prepared(...parameters_and_body) {
    const generator = new GeneratorFunction(...parameters_and_body);
    jit.prepare(generator);
    return generator;
}

function compile(generator, warm_up) {
    for (let i = 0; i < 10; ++i) warm_up();
    expect(jit.compile(generator)).toBe(jit.enabled);
}

describe("generators in compiled code", () => {
    test("resume where they yielded, with the values live across their yields", () => {
        const count = prepared(
            "start",
            "step",
            "let total = 0; for (let i = start; i < start + 5; i += step) { total += i; yield [i, total, jit.inCompiledCode()]; } return total * 2;"
        );
        compile(count, () => [...count(0, 1)]);
        const generator = count(10, 2);
        const results = [];
        for (let result = generator.next(); !result.done; result = generator.next()) results.push(result.value);
        expect(results.map(([value, total]) => [value, total])).toEqual([
            [10, 10],
            [12, 22],
            [14, 36],
        ]);
        if (jit.enabled && !jit.forcesExits) expect(results.every(([, , compiled]) => compiled)).toBeTrue();
        const first = count(1, 1).next();
        expect(first.done).toBeFalse();
        expect(first.value.slice(0, 2)).toEqual([1, 1]);
        const finished = count(0, 4);
        finished.next();
        finished.next();
        expect(finished.next()).toEqual({ value: 8, done: true });
        expect(finished.next()).toEqual({ value: undefined, done: true });
    });

    test("receive the values passed to next", () => {
        const accumulate = prepared(
            "let total = 0; while (true) { const value = yield total; if (value === undefined) return total; total += value; }"
        );
        compile(accumulate, () => {
            const generator = accumulate();
            generator.next();
            generator.next(1);
            generator.next(2);
            generator.next();
        });
        const generator = accumulate();
        expect(generator.next(100)).toEqual({ value: 0, done: false });
        expect(generator.next(5)).toEqual({ value: 5, done: false });
        expect(generator.next(0.5)).toEqual({ value: 5.5, done: false });
        expect(generator.next("x")).toEqual({ value: "5.5x", done: false });
        expect(generator.next()).toEqual({ value: "5.5x", done: true });
    });

    test("keep this, arguments and captured bindings across yields", () => {
        const make = new Function(
            "GeneratorFunction",
            "jit",
            "let captured = 1; const generator = function* (a, b) { yield this.x + a; captured += b; yield arguments.length + captured; yield this.x * b; }; jit.prepare(generator); return [generator, () => captured];"
        );
        const [generator, captured] = make(GeneratorFunction, jit);
        compile(generator, () => [...generator.call({ x: 1 }, 2, 3)]);
        const before = captured();
        expect([...generator.call({ x: 10 }, 20, 30, 40)]).toEqual([30, 3 + before + 30, 300]);
        expect(captured()).toBe(before + 30);
    });

    test("finish through return and throw", () => {
        const guarded = prepared("let i = 0; while (true) yield i++;");
        compile(guarded, () => {
            const generator = guarded();
            generator.next();
            generator.next();
            generator.return(5);
        });
        const returned = guarded();
        expect(returned.next()).toEqual({ value: 0, done: false });
        expect(returned.next()).toEqual({ value: 1, done: false });
        expect(returned.return(7)).toEqual({ value: 7, done: true });
        expect(returned.next()).toEqual({ value: undefined, done: true });

        const thrown = guarded();
        thrown.next();
        expect(() => thrown.throw(new RangeError("stop"))).toThrowWithMessage(RangeError, "stop");
        expect(thrown.next()).toEqual({ value: undefined, done: true });
    });

    test("delegate with yield*", () => {
        const inner = prepared("value", "const received = yield value; yield received * 2; return value + 1;");
        const outer = prepared(
            "inner",
            "let results = []; for (let i = 0; i < 3; ++i) results.push(yield* inner(i)); return results;"
        );
        compile(inner, () => [...inner(1)]);
        compile(outer, () => [...outer(inner)]);
        const generator = outer(inner);
        const values = [];
        let result = generator.next();
        for (let i = 0; !result.done; ++i) {
            values.push(result.value);
            result = generator.next(i);
        }
        expect(values).toEqual([0, 0, 1, 4, 2, 8]);
        expect(result).toEqual({ value: [1, 2, 3], done: true });
    });

    test("run in for-of loops and spreads", () => {
        const range = prepared("count", "for (let i = 0; i < count; ++i) yield i * i;");
        compile(range, () => [...range(3)]);
        let total = 0;
        for (const value of range(100)) total += value;
        expect(total).toBe(328350);
        expect([...range(4)]).toEqual([0, 1, 4, 9]);
        expect(Array.from(range(0))).toEqual([]);
    });

    test("leave compiled code where a speculation fails, and keep their state", () => {
        const add = prepared("a", "b", "let sum = a; for (let i = 0; i < 3; ++i) { sum = sum + b; yield sum; }");
        compile(add, () => [...add(1, 2)]);
        expect([...add(1, 2)]).toEqual([3, 5, 7]);
        expect([...add(2147483647, 1)]).toEqual([2147483648, 2147483649, 2147483650]);
        expect([...add("a", 1)]).toEqual(["a1", "a11", "a111"]);
        const generator = add(1, 1);
        expect(generator.next()).toEqual({ value: 2, done: false });
        expect(generator.next()).toEqual({ value: 3, done: false });
        expect(generator.next()).toEqual({ value: 4, done: false });
        expect(generator.next()).toEqual({ value: undefined, done: true });
    });

    test("suspended generators resume after their code is discarded and compiled again", () => {
        const values = prepared("for (let i = 0; i < 4; ++i) yield i;");
        compile(values, () => [...values()]);
        const generator = values();
        expect(generator.next()).toEqual({ value: 0, done: false });
        jit.discard(values);
        expect(generator.next()).toEqual({ value: 1, done: false });
        expect(jit.compile(values)).toBe(jit.enabled);
        expect(generator.next()).toEqual({ value: 2, done: false });
        expect(generator.next()).toEqual({ value: 3, done: false });
        expect(generator.next()).toEqual({ value: undefined, done: true });
    });

    test("loops in generators continue in compiled code at their back edges", () => {
        const sum = prepared(
            "count",
            "let total = 0; for (let i = 0; i < count; ++i) total += i % 7; yield total; yield total + 1;"
        );
        for (let i = 0; i < 10; ++i) [...sum(10)];
        const generator = sum(100000);
        let expected = 0;
        for (let i = 0; i < 100000; ++i) expected += i % 7;
        expect(generator.next()).toEqual({ value: expected, done: false });
        expect(generator.next()).toEqual({ value: expected + 1, done: false });
        expect(generator.next()).toEqual({ value: undefined, done: true });
    });
});
