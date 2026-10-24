describe("basic usage", () => {
    asyncTest("disposes asynchronously in reverse order after block exit", async () => {
        const disposed = [];
        {
            await using a = {
                async [Symbol.asyncDispose]() {
                    disposed.push("a");
                },
            };
            using b = {
                [Symbol.dispose]() {
                    disposed.push("b");
                },
            };
            await using c = {
                [Symbol.dispose]() {
                    disposed.push("c");
                },
            };
            expect(disposed).toEqual([]);
        }
        expect(disposed).toEqual(["c", "b", "a"]);
    });

    asyncTest("waits for the promise of the dispose method", async () => {
        let resolved = false;
        {
            await using a = {
                [Symbol.asyncDispose]() {
                    return Promise.resolve().then(() => {
                        resolved = true;
                    });
                },
            };
        }
        expect(resolved).toBeTrue();
    });

    asyncTest("disposes at the end of each iteration of a for-of loop", async () => {
        const disposed = [];
        for (await using x of [{ [Symbol.asyncDispose]: async () => disposed.push(1) }, null]) {
            disposed.push("body");
        }
        expect(disposed).toEqual(["body", 1, "body"]);
    });

    asyncTest("disposes after return", async () => {
        const disposed = [];
        async function f() {
            await using a = {
                async [Symbol.asyncDispose]() {
                    disposed.push("a");
                },
            };
            return "result";
        }
        expect(await f()).toBe("result");
        expect(disposed).toEqual(["a"]);
    });
});

describe("behavior with exceptions", () => {
    asyncTest("rejected dispose promise is thrown", async () => {
        const error = new Error("dispose");
        let caught;
        try {
            await using a = {
                [Symbol.asyncDispose]() {
                    return Promise.reject(error);
                },
            };
        } catch (e) {
            caught = e;
        }
        expect(caught).toBe(error);
    });

    asyncTest("errors of the block and of the dispose methods give chained suppressed errors", async () => {
        let caught;
        try {
            await using a = {
                async [Symbol.asyncDispose]() {
                    throw 1;
                },
            };
            using b = {
                [Symbol.dispose]() {
                    throw 2;
                },
            };
            throw 3;
        } catch (e) {
            caught = e;
        }
        expect(caught).toBeInstanceOf(SuppressedError);
        expect(caught.error).toBe(1);
        expect(caught.suppressed).toBeInstanceOf(SuppressedError);
        expect(caught.suppressed.error).toBe(2);
        expect(caught.suppressed.suppressed).toBe(3);
    });

    asyncTest("object without dispose method throws", async () => {
        let caught;
        try {
            await using a = {};
        } catch (e) {
            caught = e;
        }
        expect(caught).toBeInstanceOf(TypeError);
    });
});

describe("async generators", () => {
    asyncTest("return() while suspended in a scope disposes of its resources", async () => {
        const log = [];
        async function* generator() {
            await using a = {
                async [Symbol.asyncDispose]() {
                    log.push("a");
                    await null;
                    log.push("a done");
                },
            };
            try {
                await using b = {
                    [Symbol.dispose]() {
                        log.push("b");
                    },
                };
                yield 1;
            } finally {
                log.push("finally");
            }
        }

        let iterator = generator();
        await iterator.next();
        expect(await iterator.return(5)).toEqual({ value: 5, done: true });
        expect(log).toEqual(["b", "finally", "a", "a done"]);

        log.length = 0;
        iterator = generator();
        await iterator.next();
        let caught;
        try {
            await iterator.throw("thrown");
        } catch (error) {
            caught = error;
        }
        expect(caught).toBe("thrown");
        expect(log).toEqual(["b", "finally", "a", "a done"]);
    });
});

describe("syntax", () => {
    test("is only a declaration in async code", () => {
        expect("async function f() { await using a = null; }").toEval();
        expect("function f() { await using a = null; }").not.toEval();
        expect("async function f() { await using\na = null; }").toEval();
        expect("async function f() { await\nusing a = null; }").not.toEval();
    });

    test("cannot be used directly in switch cases", () => {
        expect("async function f() { switch (1) { case 1: await using a = null; } }").not.toEval();
        expect("async function f() { switch (1) { case 1: { await using a = null; } } }").toEval();
    });
});
