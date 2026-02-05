describe("correct behavior", () => {
    async function* emptyGeneratorFunction() {}

    test("length is 1", () => {
        expect(emptyGeneratorFunction.prototype.return).toHaveLength(1);
    });

    const emptyGenerator = emptyGeneratorFunction();

    test("return from SuspendedStart", () => {
        emptyGenerator
            .return(1337)
            .then(result => {
                expect(result.value).toBe(1337);
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();
    });

    test("return from Completed", () => {
        emptyGenerator
            .return(123)
            .then(result => {
                expect(result.value).toBe(123);
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();
    });

    async function* generatorTwo() {
        yield 1337;
        yield 123;
    }

    const generatorTwoIterator = generatorTwo();

    test("return from SuspendedYield", () => {
        generatorTwoIterator
            .next("bad1")
            .then(result => {
                expect(result.value).toBe(1337);
                expect(result.done).toBeFalse();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        generatorTwoIterator
            .return(999)
            .then(result => {
                expect(result.value).toBe(999);
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        generatorTwoIterator
            .next("bad2")
            .then(result => {
                expect(result.value).toBeUndefined();
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();
    });

    async function* injectedCompletionGenerator() {
        try {
            yield 1;
        } finally {
            yield 2;
        }
    }

    const injectedCompletionGeneratorObject = injectedCompletionGenerator();

    test("return completion is injected into generator", () => {
        injectedCompletionGeneratorObject
            .next("bad1")
            .then(result => {
                expect(result.value).toBe(1);
                expect(result.done).toBeFalse();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        injectedCompletionGeneratorObject
            .return(3)
            .then(result => {
                expect(result.value).toBe(2);
                expect(result.done).toBeFalse();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        injectedCompletionGeneratorObject
            .next("bad3")
            .then(result => {
                expect(result.value).toBe(3);
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        injectedCompletionGeneratorObject
            .next("bad4")
            .then(result => {
                expect(result.value).toBeUndefined();
                expect(result.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();
    });
});

describe("errors", () => {
    test("this value must be an AsyncGenerator object", () => {
        async function* generator() {}
        let rejection = null;
        generator.prototype.return.call("foo").catch(error => {
            rejection = error;
        });
        runQueuedPromiseJobs();
        expect(rejection).toBeInstanceOf(TypeError);
        expect(rejection.message).toBe("Not an object of type AsyncGenerator");
    });

    // https://github.com/tc39/ecma262/pull/2683
    test("doesn't crash on broken promises", () => {
        const promise = Promise.resolve(1337);
        Object.defineProperty(promise, "constructor", {
            get: function () {
                throw new Error("yaksplode");
            },
        });

        async function* generator() {}
        const generatorObject = generator();

        let rejection = null;
        generatorObject.return(promise).catch(error => {
            rejection = error;
        });
        runQueuedPromiseJobs();
        expect(rejection).toBeInstanceOf(Error);
        expect(rejection.message).toBe("yaksplode");
    });
});
