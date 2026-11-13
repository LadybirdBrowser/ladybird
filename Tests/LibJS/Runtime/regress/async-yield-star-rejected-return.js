describe("async yield* with a rejected return value", () => {
    for (const method of ["next", "throw", "return"]) {
        test(`forwards the rejection after yielding from ${method}()`, () => {
            const reason = {};
            const calls = [];
            const delegate = {
                [Symbol.asyncIterator]() {
                    return this;
                },
                next() {
                    calls.push("next");
                    return { value: "next", done: false };
                },
                throw(value) {
                    calls.push(["throw", value]);
                    return { value: "handled", done: value === reason };
                },
                return(value) {
                    calls.push(["return", value]);
                    return { value: "return", done: false };
                },
            };
            async function* outer() {
                const value = yield* delegate;
                calls.push(["resumed", value]);
                return "finished";
            }

            let result;
            let rejection;
            (async () => {
                const iterator = outer();
                await iterator.next();
                if (method !== "next") {
                    await iterator[method]("continue");
                }
                return iterator.return(Promise.reject(reason));
            })().then(
                value => (result = value),
                error => (rejection = error)
            );
            runQueuedPromiseJobs();

            expect(rejection).toBeUndefined();
            expect(result).toEqual({ value: "finished", done: true });
            const expected = ["next"];
            if (method !== "next") {
                expected.push([method, "continue"]);
            }
            expected.push(["throw", reason], ["resumed", "handled"]);
            expect(calls).toEqual(expected);
        });
    }

    test("can yield again from the delegated throw() method", () => {
        const reason = {};
        const calls = [];
        let nextCount = 0;
        const delegate = {
            [Symbol.asyncIterator]() {
                return this;
            },
            next(value) {
                calls.push(["next", value]);
                return { value: ++nextCount, done: nextCount === 2 };
            },
            throw(value) {
                calls.push(["throw", value]);
                return { value: "recovered", done: false };
            },
        };
        async function* outer() {
            return yield* delegate;
        }

        let results;
        let rejection;
        (async () => {
            const iterator = outer();
            await iterator.next();
            return [await iterator.return(Promise.reject(reason)), await iterator.next("continue")];
        })().then(
            value => (results = value),
            error => (rejection = error)
        );
        runQueuedPromiseJobs();

        expect(rejection).toBeUndefined();
        expect(results).toEqual([
            { value: "recovered", done: false },
            { value: 2, done: true },
        ]);
        expect(calls).toEqual([
            ["next", undefined],
            ["throw", reason],
            ["next", "continue"],
        ]);
    });

    for (const cleanupThrows of [false, true]) {
        test(`awaits cleanup when throw() is missing and cleanup ${cleanupThrows ? "throws" : "succeeds"}`, () => {
            const reason = {};
            const cleanupError = {};
            const calls = [];
            const delegate = {
                [Symbol.asyncIterator]() {
                    return this;
                },
                next() {
                    return { value: 1, done: false };
                },
                async return() {
                    calls.push("cleanup started");
                    await Promise.resolve();
                    calls.push("cleanup finished");
                    if (cleanupThrows) {
                        throw cleanupError;
                    }
                    return { done: true };
                },
            };
            async function* outer() {
                yield* delegate;
            }

            let rejection;
            (async () => {
                const iterator = outer();
                await iterator.next();
                return iterator.return(Promise.reject(reason));
            })().catch(error => {
                calls.push("rejected");
                rejection = error;
            });
            runQueuedPromiseJobs();

            expect(calls).toEqual(["cleanup started", "cleanup finished", "rejected"]);
            if (cleanupThrows) {
                expect(rejection).toBe(cleanupError);
            } else {
                expect(rejection).toBeInstanceOf(TypeError);
            }
        });
    }

    test("a rejected return still throws into an ordinary async yield", () => {
        const reason = {};
        let caught;
        async function* generator() {
            try {
                yield 1;
            } catch (error) {
                caught = error;
                yield "recovered";
            }
        }

        let result;
        let rejection;
        (async () => {
            const iterator = generator();
            await iterator.next();
            return iterator.return(Promise.reject(reason));
        })().then(
            value => (result = value),
            error => (rejection = error)
        );
        runQueuedPromiseJobs();

        expect(rejection).toBeUndefined();
        expect(caught).toBe(reason);
        expect(result).toEqual({ value: "recovered", done: false });
    });
});
