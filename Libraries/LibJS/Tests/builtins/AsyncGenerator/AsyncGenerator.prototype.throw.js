describe("correct behavior", () => {
    async function* emptyGeneratorFunction() {}

    test("length is 1", () => {
        expect(emptyGeneratorFunction.prototype.throw).toHaveLength(1);
    });

    const emptyGenerator = emptyGeneratorFunction();

    test("throw from SuspendedStart", () => {
        emptyGenerator
            .throw(1337)
            .then(() => {
                expect().fail("Generator did NOT throw an unhandled exception.");
            })
            .catch(e => {
                expect(e).toBe(1337);
            });
        runQueuedPromiseJobs();
    });

    test("throw from Completed", () => {
        emptyGenerator
            .throw(123)
            .then(() => {
                expect().fail("Generator did NOT throw an unhandled exception.");
            })
            .catch(e => {
                expect(e).toBe(123);
            });
        runQueuedPromiseJobs();
    });

    async function* generatorTwo() {
        yield 1337;
        yield 123;
    }

    const generatorTwoIterator = generatorTwo();

    test("throw from SuspendedYield", () => {
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
            .throw(999)
            .then(() => {
                expect().fail("Generator did NOT throw an unhandled exception.");
            })
            .catch(e => {
                expect(e).toBe(999);
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
        } catch (e) {
            yield e;
        }
    }

    const injectedCompletionGeneratorObject = injectedCompletionGenerator();

    test("throw completion is injected into generator", () => {
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
            .throw(9999)
            .then(result => {
                expect(result.value).toBe(9999);
                expect(result.done).toBeFalse();
            })
            .catch(e => {
                expect().fail(`Generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();

        injectedCompletionGeneratorObject
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
});

describe("errors", () => {
    test("this value must be an AsyncGenerator object", () => {
        async function* generator() {}
        let rejection = null;
        generator.prototype.throw.call("foo").catch(error => {
            rejection = error;
        });
        runQueuedPromiseJobs();
        expect(rejection).toBeInstanceOf(TypeError);
        expect(rejection.message).toBe("Not an object of type AsyncGenerator");
    });
});
