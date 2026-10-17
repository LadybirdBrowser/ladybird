// Async functions and async generators run in compiled code: a call starts one there, and each resumption enters the
// code at the continuation of the await it was suspended at, with the values that the frame kept across the
// suspension.
//
// NB: The functions come from the AsyncFunction and AsyncGeneratorFunction constructors, so that each run of a test
//     (see LIBJS_TEST_REPEAT) compiles new ones.

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const AsyncGeneratorFunction = Object.getPrototypeOf(async function* () {}).constructor;

function prepared(constructor, ...parameters_and_body) {
    const function_ = new constructor(...parameters_and_body);
    jit.prepare(function_);
    return function_;
}

function settle(promise) {
    let result;
    promise.then(
        value => (result = { value }),
        error => (result = { error })
    );
    runQueuedPromiseJobs();
    return result;
}

function compile(function_, warm_up) {
    for (let i = 0; i < 10; ++i) warm_up();
    runQueuedPromiseJobs();
    expect(jit.compile(function_)).toBe(jit.enabled);
}

describe("async functions in compiled code", () => {
    test("resume where they awaited, with the values live across their awaits", () => {
        const count = prepared(
            AsyncFunction,
            "start",
            "step",
            "let total = 0; const steps = []; for (let i = start; i < start + 5; i += step) { total += await i; steps.push([i, total, jit.inCompiledCode()]); } return [total * 2, steps];"
        );
        compile(count, () => count(0, 1));
        const { value } = settle(count(10, 2));
        const [doubled, steps] = value;
        expect(doubled).toBe(72);
        expect(steps.map(([i, total]) => [i, total])).toEqual([
            [10, 10],
            [12, 22],
            [14, 36],
        ]);
        if (jit.enabled && !jit.forcesExits) expect(steps.every(([, , compiled]) => compiled)).toBeTrue();
    });

    test("receive the values of the promises they await", () => {
        const add = prepared(
            AsyncFunction,
            "a",
            "b",
            "const x = await a; const y = await Promise.resolve(b); return x + y + (await { valueOf: () => 0 }).valueOf();"
        );
        compile(add, () => add(1, 2));
        expect(settle(add(3, 4))).toEqual({ value: 7 });
        expect(settle(add(Promise.resolve("x"), "y"))).toEqual({ value: "xy0" });
        expect(settle(add(1.5, Promise.resolve(2)))).toEqual({ value: 3.5 });
    });

    test("wait for pending promises", () => {
        const wait = prepared(
            AsyncFunction,
            "promise",
            "suffix",
            "const value = await promise; return value + suffix;"
        );
        compile(wait, () => wait(Promise.resolve(1), 1));
        let resolve;
        const promise = new Promise(r => (resolve = r));
        let result;
        wait(promise, "!").then(value => (result = value));
        runQueuedPromiseJobs();
        expect(result).toBeUndefined();
        resolve("done");
        runQueuedPromiseJobs();
        expect(result).toBe("done!");
    });

    test("reject when an awaited promise rejects or they throw", () => {
        const rethrow = prepared(
            AsyncFunction,
            "promise",
            "fail",
            "const value = await promise; if (fail) throw new TypeError(value); return value;"
        );
        compile(rethrow, () => rethrow(1, false));
        expect(settle(rethrow(5, false))).toEqual({ value: 5 });
        const rejected = settle(rethrow(Promise.reject(new RangeError("rejected")), false));
        expect(rejected.error).toBeInstanceOf(RangeError);
        expect(rejected.error.message).toBe("rejected");
        const thrown = settle(rethrow("thrown", true));
        expect(thrown.error).toBeInstanceOf(TypeError);
        expect(thrown.error.message).toBe("thrown");
    });

    test("keep this, arguments and captured bindings across awaits", () => {
        const make = new Function(
            "AsyncFunction",
            "jit",
            "let captured = 1; const async_function = async function (a, b) { const x = this.x + (await a); captured += await b; return [x, arguments.length + captured, this.x * b]; }; jit.prepare(async_function); return [async_function, () => captured];"
        );
        const [async_function, captured] = make(AsyncFunction, jit);
        compile(async_function, () => async_function.call({ x: 1 }, 2, 3));
        const before = captured();
        expect(settle(async_function.call({ x: 10 }, 20, 30, 40))).toEqual({ value: [30, 3 + before + 30, 300] });
        expect(captured()).toBe(before + 30);
    });

    test("leave compiled code where a speculation fails, and keep their state", () => {
        const add = prepared(
            AsyncFunction,
            "a",
            "b",
            "let sum = a; const sums = []; for (let i = 0; i < 3; ++i) { sum = sum + (await b); sums.push(sum); } return sums;"
        );
        compile(add, () => add(1, 2));
        expect(settle(add(1, 2))).toEqual({ value: [3, 5, 7] });
        expect(settle(add(2147483647, 1))).toEqual({ value: [2147483648, 2147483649, 2147483650] });
        expect(settle(add("a", 1))).toEqual({ value: ["a1", "a11", "a111"] });
    });

    test("suspended async functions resume after their code is discarded and compiled again", () => {
        const values = prepared(
            AsyncFunction,
            "promises",
            "const results = []; for (const promise of promises) results.push(await promise); return results;"
        );
        compile(values, () => values([1, 2]));
        const resolvers = [];
        const promises = [0, 1, 2].map(() => new Promise(resolve => resolvers.push(resolve)));
        let result;
        values(promises).then(value => (result = value));
        resolvers[0]("a");
        runQueuedPromiseJobs();
        jit.discard(values);
        resolvers[1]("b");
        runQueuedPromiseJobs();
        expect(jit.compile(values)).toBe(jit.enabled);
        resolvers[2]("c");
        runQueuedPromiseJobs();
        expect(result).toEqual(["a", "b", "c"]);
    });

    test("compile async arrow functions and async methods once they get hot", () => {
        const make = new Function(
            "return [async (x) => (await x) * 2, { async method(x) { return (await x) + this.offset; }, offset: 10 }];"
        );
        const [arrow, object] = make();
        let total = 0;
        for (let i = 0; i < 1000; ++i) {
            arrow(i).then(value => (total += value));
            object.method(i).then(value => (total += value));
        }
        runQueuedPromiseJobs();
        expect(total).toBe(999000 + 499500 + 10000);
    });
});

describe("async functions run in their call until they first await", () => {
    test("reject when evaluating a parameter throws", () => {
        const parameters = prepared(AsyncFunction, "a = (() => { throw new TypeError('parameter'); })()", "return a;");
        compile(parameters, () => parameters(1));
        let promise;
        expect(() => (promise = parameters())).not.toThrow();
        const { error } = settle(promise);
        expect(error).toBeInstanceOf(TypeError);
        expect(error.message).toBe("parameter");
        expect(settle(parameters(5))).toEqual({ value: 5 });
    });

    test("run synchronously up to the first await, and reject when they throw before it", () => {
        const steps = [];
        const make = new Function(
            "steps",
            "jit",
            "const f = async function (fail) { steps.push('body'); if (fail) throw new RangeError('before'); await 0; steps.push('after'); }; jit.prepare(f); return f;"
        );
        const f = make(steps, jit);
        compile(f, () => f(false));
        steps.length = 0;
        const promise = f(false);
        steps.push("returned");
        runQueuedPromiseJobs();
        expect(steps).toEqual(["body", "returned", "after"]);
        let thrown;
        expect(() => (thrown = f(true))).not.toThrow();
        const { error } = settle(thrown);
        expect(error).toBeInstanceOf(RangeError);
        expect(error.message).toBe("before");
        expect(settle(promise)).toEqual({ value: undefined });
    });

    test("resolve when they return without awaiting, adopting returned thenables", () => {
        const identity = prepared(AsyncFunction, "value", "return value;");
        compile(identity, () => identity(1));
        expect(settle(identity(7))).toEqual({ value: 7 });
        expect(settle(identity())).toEqual({ value: undefined });
        const thenable = { then: resolve => resolve("adopted") };
        expect(settle(identity(thenable))).toEqual({ value: "adopted" });
        expect(settle(identity(Promise.resolve("promise")))).toEqual({ value: "promise" });
        const rejected = settle(identity(Promise.reject(new SyntaxError("rejected"))));
        expect(rejected.error).toBeInstanceOf(SyntaxError);
    });

    test("catch a rejection of their first await", () => {
        const guarded = prepared(
            AsyncFunction,
            "promise",
            "try { await promise; return 'resolved'; } catch (error) { return 'caught ' + error; }"
        );
        expect(settle(guarded(Promise.reject("oops")))).toEqual({ value: "caught oops" });
        expect(settle(guarded(Promise.resolve()))).toEqual({ value: "resolved" });
    });

    test("show up once in stack traces before their first await", () => {
        async function listsItselfOnce() {
            const stack = new Error().stack;
            await 0;
            return stack;
        }
        const { value } = settle(listsItselfOnce());
        expect(value.split("\n").filter(line => line.includes("listsItselfOnce"))).toHaveLength(1);
    });
});

describe("async generators in compiled code", () => {
    test("yield and await with the values live across both", () => {
        const values = prepared(
            AsyncGeneratorFunction,
            "count",
            "let total = 0; for (let i = 0; i < count; ++i) { total += await i; yield [i, total, jit.inCompiledCode()]; } return total;"
        );
        const collect = async generator => {
            const results = [];
            for await (const value of generator) results.push(value);
            return results;
        };
        compile(values, () => collect(values(3)));
        const { value } = settle(collect(values(4)));
        expect(value.map(([i, total]) => [i, total])).toEqual([
            [0, 0],
            [1, 1],
            [2, 3],
            [3, 6],
        ]);
        if (jit.enabled && !jit.forcesExits) expect(value.every(([, , compiled]) => compiled)).toBeTrue();

        const generator = values(2);
        const first = settle(generator.next()).value;
        expect(first.done).toBeFalse();
        expect(first.value.slice(0, 2)).toEqual([0, 0]);
        settle(generator.next());
        expect(settle(generator.next()).value).toEqual({ value: 1, done: true });
    });

    test("receive the values passed to next and finish through return", () => {
        const accumulate = prepared(
            AsyncGeneratorFunction,
            "let total = 0; while (true) { const value = await (yield total); if (value === undefined) return total; total += value; }"
        );
        compile(accumulate, () => {
            const generator = accumulate();
            generator.next();
            generator.next(1);
            generator.next();
        });
        const generator = accumulate();
        expect(settle(generator.next(100)).value).toEqual({ value: 0, done: false });
        expect(settle(generator.next(Promise.resolve(5))).value).toEqual({ value: 5, done: false });
        expect(settle(generator.next(0.5)).value).toEqual({ value: 5.5, done: false });
        expect(settle(generator.next()).value).toEqual({ value: 5.5, done: true });

        const returned = accumulate();
        settle(returned.next());
        expect(settle(returned.return(7)).value).toEqual({ value: 7, done: true });
        expect(settle(returned.next()).value).toEqual({ value: undefined, done: true });
    });
});
