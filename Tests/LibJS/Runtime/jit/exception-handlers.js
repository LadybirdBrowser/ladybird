// Exception handlers run in compiled code: an exception thrown in compiled code leaves it with every value live into
// the handler written to the frame, the interpreter runs the handler's Catch, and the frame continues in compiled code
// after it. Finally blocks are handlers too, and so are the handlers that close iterators.
//
// NB: The functions come from the Function constructor, so that each run of a test (see LIBJS_TEST_REPEAT) compiles
//     new ones.

function prepared(...parameters_and_body) {
    const make = new Function(
        "jit",
        "thrower",
        `const f = function (${parameters_and_body.slice(0, -1).join(", ")}) { ${parameters_and_body.at(-1)} }; jit.prepare(f); return f;`
    );
    return make(jit, thrower);
}

function thrower(value) {
    if (value & 1) throw value;
    return value;
}

function compile(f, warm_up) {
    for (let i = 0; i < 10; ++i) warm_up();
    expect(jit.compile(f)).toBe(jit.enabled);
}

const checksCompiledCode = () => jit.enabled && !jit.forcesExits;

describe("exception handlers in compiled code", () => {
    test("catch exceptions of calls, with the values live into the handler", () => {
        const sum = prepared(
            "n",
            "let total = 0; let caught = 0; let compiled = true; for (let i = 0; i < n; ++i) { const before = total; try { total += 100; total += thrower(i); } catch (e) { total = before - e; ++caught; compiled &&= jit.inCompiledCode(); } } return [total, caught, compiled];"
        );
        compile(sum, () => sum(10));
        const [total, caught, compiled] = sum(1000);
        let expected = 0;
        for (let i = 0; i < 1000; ++i) expected = i & 1 ? expected - i : expected + 100 + i;
        expect(total).toBe(expected);
        expect(caught).toBe(500);
        if (checksCompiledCode()) expect(compiled).toBeTrue();
    });

    test("run finally blocks after normal completions, returns, breaks and exceptions", () => {
        const run = prepared(
            "n",
            "const log = []; for (let i = 0; i < n; ++i) { try { if (i % 4 === 1) continue; if (i % 4 === 2) break; thrower(i); log.push('try ' + i); } catch (e) { log.push('catch ' + e); } finally { log.push('finally ' + i); } } return log;"
        );
        const returns = prepared(
            "x",
            "let state = 'start'; try { state = 'try'; if (x) return state; thrower(1); } catch (e) { state = 'catch ' + e; return state; } finally { state += ' finally'; } return state;"
        );
        compile(run, () => run(3));
        compile(returns, () => [returns(true), returns(false)]);
        expect(run(2)).toEqual(["try 0", "finally 0", "finally 1"]);
        expect(run(10)).toEqual(["try 0", "finally 0", "finally 1", "finally 2"]);
        expect(returns(true)).toBe("try");
        expect(returns(false)).toBe("catch 1");
    });

    test("rethrow from catch and finally blocks to outer handlers", () => {
        const nested = prepared(
            "n",
            "let log = ''; for (let i = 0; i < n; ++i) { try { try { thrower(i); log += 'a'; } catch (e) { log += 'b'; throw e + 1; } finally { log += 'c'; } } catch (e) { log += e; } } return log;"
        );
        const propagates = prepared("x", "try { thrower(x); } finally { if (x === 3) return 'overridden'; }");
        compile(nested, () => nested(4));
        compile(propagates, () => {
            try {
                propagates(1);
            } catch {}
            propagates(2);
        });
        expect(nested(6)).toBe("acbc2acbc4acbc6");
        let thrown;
        try {
            propagates(5);
        } catch (e) {
            thrown = e;
        }
        expect(thrown).toBe(5);
        expect(propagates(3)).toBe("overridden");
        expect(propagates(4)).toBeUndefined();
    });

    test("catch exceptions thrown by property accesses, builtins and checks", () => {
        const access = prepared(
            "values",
            "const results = []; for (const value of values) { try { results.push(value.x.y); } catch (e) { results.push(e.constructor.name); } try { results.push(JSON.parse(value.text)); } catch (e) { results.push(e.constructor.name); } } return results;"
        );
        const getter = {
            get x() {
                throw new RangeError();
            },
        };
        compile(access, () =>
            access([
                { x: { y: 1 }, text: "1" },
                { x: null, text: "{" },
            ])
        );
        expect(access([{ x: { y: 2 }, text: "[3]" }, {}, getter, { x: { y: 4 }, text: "}" }])).toEqual([
            2,
            [3],
            "TypeError",
            "SyntaxError",
            "RangeError",
            "SyntaxError",
            4,
            "SyntaxError",
        ]);
    });

    test("catch exceptions thrown with throw statements in the same function", () => {
        const classify = prepared(
            "values",
            "let total = 0; for (const value of values) { try { if (typeof value !== 'number') throw value; total += value; } catch (e) { total += e.length; } } return total;"
        );
        compile(classify, () => classify([1, "ab", 2]));
        expect(classify([1, 2, "abc", 3, "de", [1, 2, 3, 4]])).toBe(15);
    });

    test("handle exceptions in handlers that first catch after the code was compiled", () => {
        const late = prepared(
            "n, limit",
            "let total = 0; for (let i = 0; i < n; ++i) { try { if (i >= limit) throw i; total += i; } catch (e) { total -= e; } } return total;"
        );
        compile(late, () => late(10, 100));
        let expected = 0;
        for (let i = 0; i < 300; ++i) expected += i < 50 ? i : -i;
        for (let run = 0; run < 20; ++run) expect(late(300, 50)).toBe(expected);
    });

    test("close iterators of loops left by exceptions", () => {
        const closes = prepared(
            "iterable",
            "let total = 0; try { for (const value of iterable) { total += thrower(value); } } catch (e) { total += 1000 * e; } return total;"
        );
        let closed = 0;
        const iterable = values => ({
            [Symbol.iterator]() {
                let index = 0;
                return {
                    next: () => (index < values.length ? { value: values[index++], done: false } : { done: true }),
                    return: () => {
                        ++closed;
                        return {};
                    },
                };
            },
        });
        compile(closes, () => [closes(iterable([2, 4])), closes(iterable([2, 3]))]);
        closed = 0;
        expect(closes(iterable([2, 4, 6]))).toBe(12);
        expect(closes(iterable([2, 4, 5, 6]))).toBe(5006);
        expect(closed).toBe(1);
    });
    test("keep unboxed values live into handlers", () => {
        const mixed = prepared(
            "n",
            "let count = 0; let ratio = 0.5; let flag = false; let log = []; for (let i = 0; i < n; ++i) { count += 3; ratio *= 1.5; flag = !flag; try { thrower(i); } catch (e) { log.push([count, ratio, flag, e]); } } return log;"
        );
        compile(mixed, () => mixed(4));
        const expected = [];
        let count = 0;
        let ratio = 0.5;
        let flag = false;
        for (let i = 0; i < 6; ++i) {
            count += 3;
            ratio *= 1.5;
            flag = !flag;
            if (i & 1) expected.push([count, ratio, flag, i]);
        }
        expect(mixed(6)).toEqual(expected);
    });

    test("catch exceptions thrown before the frame was observed", () => {
        const first = prepared("x", "try { return thrower(x); } catch (e) { return -e; }");
        compile(first, () => [first(1), first(2)]);
        const results = [];
        for (let i = 0; i < 100; ++i) results.push(first(i));
        expect(results).toEqual(results.map((_, i) => (i & 1 ? -i : i)));
    });

    test("catch exceptions thrown by calls that invalidated the code", () => {
        const make = new Function(
            "jit",
            "class Point { constructor(x) { this.x = x; } } const read = function (point, poison) { let total = 0; try { total += point.x; poison(); total += point.x; } catch (e) { total += point.x * 10 + e; } return total; }; jit.prepare(read); return [read, Point];"
        );
        const [read, Point] = make(jit);
        const harmless = () => {};
        compile(read, () => read(new Point(1), harmless));
        const point = new Point(2);
        const poison = () => {
            Object.defineProperty(Point.prototype, "x", { get: () => 100, configurable: true });
            delete point.x;
            throw 7;
        };
        expect(read(point, poison)).toBe(2 + 100 * 10 + 7);
        expect(read({ x: 3 }, harmless)).toBe(6);
    });
});

describe("exception handlers in compiled generators", () => {
    test("catch exceptions between yields and thrown into them", () => {
        const GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor;
        const generator = new GeneratorFunction(
            "thrower",
            "n",
            "for (let i = 0; i < n; ++i) { try { yield thrower(i); } catch (e) { yield 'caught ' + e; } }"
        );
        jit.prepare(generator);
        compile(generator, () => [...generator(thrower, 4)]);
        expect([...generator(thrower, 4)]).toEqual([0, "caught 1", 2, "caught 3"]);
        const resumed = generator(thrower, 2);
        expect(resumed.next()).toEqual({ value: 0, done: false });
        expect(resumed.throw("thrown")).toEqual({ value: "caught thrown", done: false });
        expect(resumed.next()).toEqual({ value: "caught 1", done: false });
        expect(resumed.next()).toEqual({ value: undefined, done: true });
    });
});
