describe("basic behavior", () => {
    test("empty array", () => {
        var enteredFunction = false;
        var rejected = false;
        async function f() {
            enteredFunction = true;
            for await (const v of []) {
                expect().fail("Should not enter loop");
            }
        }

        f().then(
            () => {
                expect(enteredFunction).toBeTrue();
            },
            () => {
                rejected = true;
            }
        );
        runQueuedPromiseJobs();
        expect(enteredFunction).toBeTrue();
        expect(rejected).toBeFalse();
    });

    test("sync iterator", () => {
        var loopIterations = 0;
        var rejected = false;
        async function f() {
            for await (const v of [1]) {
                expect(v).toBe(1);
                loopIterations++;
            }
        }

        f().then(
            () => {
                expect(loopIterations).toBe(1);
            },
            () => {
                rejected = true;
            }
        );
        runQueuedPromiseJobs();
        expect(loopIterations).toBe(1);
        expect(rejected).toBeFalse();
    });

    test("can break a for-await-of loop", () => {
        var loopIterations = 0;
        var rejected = false;
        async function f() {
            for await (const v of [1, 2, 3]) {
                expect(v).toBe(1);
                loopIterations++;
                break;
            }
        }

        f().then(
            () => {
                expect(loopIterations).toBe(1);
            },
            () => {
                rejected = true;
            }
        );
        runQueuedPromiseJobs();
        expect(loopIterations).toBe(1);
        expect(rejected).toBeFalse();
    });

    test("a result of next() that is not an object throws once it is awaited", () => {
        const log = [];
        const iterable = {
            [Symbol.asyncIterator]() {
                return {
                    next() {
                        log.push("next");
                        return 5;
                    },
                };
            },
        };
        (async () => {
            try {
                for await (const value of iterable);
            } catch (error) {
                log.push(error.name);
            }
        })();
        log.push("after the call");
        runQueuedPromiseJobs();
        expect(log).toEqual(["next", "after the call", "TypeError"]);
    });
});

describe("only allowed in async functions", () => {
    test("async functions", () => {
        expect("async function foo() { for await (const v of []) return v; }").toEval();
        expect("(async function () { for await (const v of []) return v; })").toEval();
        expect("async () => { for await (const v of []) return v; }").toEval();
        expect("async function foo() { let async; for await (async of []) ; }").toEval();
    });

    test("regular functions", () => {
        expect("function foo() { for await (const v of []) return v; }").not.toEval();
        expect("(function () { for await (const v of []) return v; })").not.toEval();
        expect("() => { for await (const v of []) return v; }").not.toEval();
    });

    test("generator functions", () => {
        expect("function* foo() { for await (const v of []) return v; }").not.toEval();
        expect("(function* () { for await (const v of []) return v; })").not.toEval();
    });

    test("async generator functions", () => {
        expect("async function* foo() { for await (const v of []) yield v; }").toEval();
        expect("(async function* () { for await (const v of []) yield v; })").toEval();
        expect("async function* foo() { let async; for await (async of []) ; }").toEval();
    });
});
