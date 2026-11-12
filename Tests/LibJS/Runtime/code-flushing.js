// The code of a function that did not run for a while is flushed, and compiled again when the function is called
// again. flushCode() flushes everything that nothing but its function holds, and code must behave the same afterwards.

// NB: The bodies of these tests never run in JIT code, which would hold the code of the functions it inlines. The
//     functions they declare keep their shared data across the harness's repeated runs of a test, so that each run
//     would add recompiles, which stop the flushes of a function after six. So the body runs once.
function flushingTest(name, body) {
    let ran = false;
    test(name, function run() {
        if (ran) return;
        ran = true;
        if (typeof jit !== "undefined") {
            jit.neverCompile(run);
            jit.neverCompile(body);
        }
        body();
    });
}

describe("flushed functions", () => {
    flushingTest("a flushed function compiles again when it is called", () => {
        function add(a, b) {
            return a + b;
        }
        expect(add(1, 2)).toBe(3);
        flushCode();
        expect(isCodeFlushed(add)).toBeTrue();
        expect(add(3, 4)).toBe(7);
        expect(isCodeFlushed(add)).toBeFalse();
    });

    flushingTest("closures keep working over the code of flushed functions", () => {
        function makeCounter(start) {
            let count = start;
            return {
                increment: () => ++count,
                read() {
                    return count;
                },
            };
        }
        const before = makeCounter(10);
        expect(before.increment()).toBe(11);
        flushCode();
        expect(isCodeFlushed(makeCounter)).toBeTrue();
        expect(isCodeFlushed(before.increment)).toBeTrue();
        expect(before.increment()).toBe(12);
        expect(before.read()).toBe(12);
        const after = makeCounter(0);
        expect(after.increment()).toBe(1);
        expect(before.read()).toBe(12);
    });

    flushingTest("template objects stay the same across recompiles", () => {
        const tag = strings => strings;
        function direct() {
            return tag`a${1}b`;
        }
        function outer() {
            return () => tag`c${2}d`;
        }
        const first = direct();
        const innerBefore = outer();
        const nestedFirst = innerBefore();
        flushCode();
        expect(isCodeFlushed(direct)).toBeTrue();
        expect(isCodeFlushed(outer)).toBeTrue();
        expect(direct()).toBe(first);
        expect(outer()()).toBe(nestedFirst);
        expect(innerBefore()).toBe(nestedFirst);
    });

    flushingTest("recompiled code gives the same results", () => {
        class Point {
            #secret = 7;
            static origin = new Point(0, 0);
            constructor(x, y) {
                this.x = x;
                this.y = y;
            }
            get sum() {
                return this.x + this.y + this.#secret;
            }
            *coordinates() {
                yield this.x;
                yield this.y;
            }
        }
        function work(values, { scale = 2 } = {}) {
            let total = 0;
            for (const [index, value] of values.entries()) {
                try {
                    if (value < 0) throw new RangeError(`negative at ${index}`);
                    total += value * scale;
                } catch (error) {
                    total -= index;
                }
            }
            const point = new Point(arguments.length, total);
            return [total, point.sum, [...point.coordinates()], Point.origin.sum, typeof eval("scale")];
        }
        const expected = JSON.stringify(work([1, -2, 3], { scale: 3 }));
        flushCode();
        expect(isCodeFlushed(work)).toBeTrue();
        expect(JSON.stringify(work([1, -2, 3], { scale: 3 }))).toBe(expected);
        flushCode();
        expect(isCodeFlushed(work)).toBeTrue();
        expect(JSON.stringify(work([1, -2, 3], { scale: 3 }))).toBe(expected);
    });

    flushingTest("exceptions report the same stack and position after recompiles", () => {
        function thrower(value) {
            if (value > 0) return thrower(value - 1);
            throw new Error("deep");
        }
        function stackOf() {
            try {
                thrower(2);
            } catch (error) {
                return error.stack;
            }
        }
        // NB: The frames below stackOf() differ only in the line it is called from.
        const framesUpToStackOf = stack => stack.split("\n").slice(0, 5).join("\n");
        const expected = framesUpToStackOf(stackOf());
        expect(expected.includes("at thrower (")).toBeTrue();
        expect(expected.includes("at stackOf (")).toBeTrue();
        flushCode();
        expect(isCodeFlushed(thrower)).toBeTrue();
        expect(framesUpToStackOf(stackOf())).toBe(expected);
    });

    flushingTest("functions are flushed six times at most", () => {
        function often() {
            return "often";
        }
        for (let i = 0; i < 6; ++i) {
            expect(often()).toBe("often");
            flushCode();
            expect(isCodeFlushed(often)).toBeTrue();
        }
        expect(often()).toBe("often");
        flushCode();
        expect(isCodeFlushed(often)).toBeFalse();
    });
});

describe("code that something else holds", () => {
    flushingTest("running functions keep their code", () => {
        function running() {
            flushCode();
            return isCodeFlushed(running);
        }
        expect(running()).toBeFalse();
    });

    flushingTest("suspended generators keep their code", () => {
        function* numbers() {
            let i = 0;
            while (true) yield i++;
        }
        const iterator = numbers();
        expect(iterator.next().value).toBe(0);
        flushCode();
        expect(isCodeFlushed(numbers)).toBeFalse();
        expect(iterator.next().value).toBe(1);
        const other = numbers();
        expect(other.next().value).toBe(0);
        expect(iterator.next().value).toBe(2);
    });

    flushingTest("suspended async functions keep their code", () => {
        let resolve;
        const promise = new Promise(resolver => (resolve = resolver));
        async function later() {
            const value = await promise;
            return value + 1;
        }
        let result;
        later().then(value => (result = value));
        flushCode();
        expect(isCodeFlushed(later)).toBeFalse();
        resolve(41);
        runQueuedPromiseJobs();
        expect(result).toBe(42);
    });

    flushingTest("finished generators let their code go", () => {
        function* once() {
            yield 1;
        }
        expect([...once()]).toEqual([1]);
        flushCode();
        expect(isCodeFlushed(once)).toBeTrue();
        expect([...once()]).toEqual([1]);
    });
});
