describe("correct behaviour", () => {
    async function* generatorFunction() {
        yield 1;
        await Promise.resolve(2);
        const b = yield 3;
        await Promise.resolve(b);
        yield b + 1;
        yield Promise.resolve(b + 2);
        yield* [Promise.resolve(b + 3), Promise.resolve(b + 4), Promise.resolve(b + 5)];
        return Promise.resolve(b + 6);
    }

    test("length is 1", () => {
        expect(generatorFunction.prototype.next).toHaveLength(1);
    });

    const generator = generatorFunction();

    function runGenerator(valueToPass, unwrapIteratorResult = true) {
        let result = null;
        test(`generator runs valueToPass=${valueToPass}`, () => {
            const promise = generator.next(valueToPass);
            promise
                .then(value => {
                    result = value;
                })
                .catch(e => {
                    expect().fail(`Generator threw an unhandled exception: ${e}`);
                });
            runQueuedPromiseJobs();
            expect(result).toBeInstanceOf(Object);
            expect(Object.getPrototypeOf(result)).toBe(Object.prototype);
            expect(Object.keys(result)).toEqual(["value", "done"]);
        });
        return unwrapIteratorResult ? result.value : result;
    }

    test("can yield", () => {
        const firstRunResult = runGenerator("bad1");
        expect(firstRunResult).toBe(1);
    });

    test("await does not yield", () => {
        const secondRunResult = runGenerator("bad2");
        expect(secondRunResult).toBe(3);
    });

    test("can pass values via yield", () => {
        const thirdRunResult = runGenerator(4);
        expect(thirdRunResult).toBe(5);
    });

    test("yield implicitly awaits", () => {
        const fourthRunResult = runGenerator("bad3");
        expect(fourthRunResult).toBe(6);

        const fifthRunResult = runGenerator("bad4");
        expect(fifthRunResult).toBe(7);

        const sixthRunResult = runGenerator("bad5");
        expect(sixthRunResult).toBe(8);

        const seventhRunResult = runGenerator("bad6");
        expect(seventhRunResult).toBe(9);
    });

    test("can return a value and return implicitly awaits", () => {
        const eighthRunResult = runGenerator("bad7", false);
        expect(eighthRunResult.value).toBe(10);
        expect(eighthRunResult.done).toBeTrue();
    });

    test("gets undefined in completed state", () => {
        const ninethRunResult = runGenerator("bad8", false);
        expect(ninethRunResult.value).toBeUndefined();
        expect(ninethRunResult.done).toBeTrue();
    });

    async function* implicitReturnFunction() {
        0xbbadbeef;
    }

    const implicitReturnGenerator = implicitReturnFunction();

    test("gets undefined from implicit return", () => {
        implicitReturnGenerator
            .next("bad9")
            .then(iteratorResult => {
                expect(iteratorResult.value).toBeUndefined();
                expect(iteratorResult.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(`Implicit await generator threw an unhandled exception: ${e}`);
            });
        runQueuedPromiseJobs();
    });

    async function* unhandledExceptionFunction() {
        throw 1337;
    }

    const unhandledExceptionGenerator = unhandledExceptionFunction();

    test("promise is rejected on unhandled exceptions", () => {
        unhandledExceptionGenerator
            .next("bad10")
            .then(() => {
                expect().fail(
                    "Unhandled exception generator did NOT throw an unhandled exception."
                );
            })
            .catch(e => {
                expect(e).toBe(1337);
            });
        runQueuedPromiseJobs();
    });

    test("generator is complete after unhandled exception", () => {
        unhandledExceptionGenerator
            .next("bad11")
            .then(iteratorResult => {
                expect(iteratorResult.value).toBeUndefined();
                expect(iteratorResult.done).toBeTrue();
            })
            .catch(e => {
                expect().fail(
                    "Unhandled exception generator threw an unhandled exception in Completed state."
                );
            });
        runQueuedPromiseJobs();
    });
});

describe("errors", () => {
    test("this value must be an AsyncGenerator object", () => {
        async function* generator() {}
        let rejection = null;
        generator.prototype.next.call("foo").catch(error => {
            rejection = error;
        });
        runQueuedPromiseJobs();
        expect(rejection).toBeInstanceOf(TypeError);
        expect(rejection.message).toBe("Not an object of type AsyncGenerator");
    });
});
