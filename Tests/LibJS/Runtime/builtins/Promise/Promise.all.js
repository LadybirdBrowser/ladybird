test("length is 1", () => {
    expect(Promise.all).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise", () => {
        const promise = Promise.all();
        expect(promise).toBeInstanceOf(Promise);
    });

    test("resolve", () => {
        const promise1 = Promise.resolve(3);
        const promise2 = 42;
        const promise3 = new Promise((resolve, reject) => {
            resolve("foo");
        });

        let resolvedValues = null;
        let wasRejected = false;

        Promise.all([promise1, promise2, promise3]).then(
            values => {
                resolvedValues = values;
            },
            () => {
                wasRejected = true;
            }
        );

        runQueuedPromiseJobs();
        expect(resolvedValues).toEqual([3, 42, "foo"]);
        expect(wasRejected).toBeFalse();
    });

    test("reject", () => {
        const promise1 = Promise.resolve(3);
        const promise2 = 42;
        const promise3 = new Promise((resolve, reject) => {
            reject("foo");
        });

        let rejectionReason = null;
        let wasResolved = false;

        Promise.all([promise1, promise2, promise3]).then(
            () => {
                wasResolved = true;
            },
            reason => {
                rejectionReason = reason;
            }
        );

        runQueuedPromiseJobs();
        expect(rejectionReason).toBe("foo");
        expect(wasResolved).toBeFalse();
    });

    test("invokes an overridden then method", () => {
        const promise = Promise.resolve(42);
        const originalThen = promise.then;
        let callCount = 0;
        let resolvedValues = null;

        promise.then = function (...args) {
            ++callCount;
            return originalThen.apply(this, args);
        };

        Promise.all([promise]).then(values => {
            resolvedValues = values;
        });

        runQueuedPromiseJobs();
        expect(callCount).toBe(1);
        expect(resolvedValues).toEqual([42]);
    });

    test("observes Symbol.species when invoking then", () => {
        const originalSpecies = Object.getOwnPropertyDescriptor(Promise, Symbol.species);
        let speciesCallCount = 0;

        Object.defineProperty(Promise, Symbol.species, {
            configurable: true,
            get() {
                ++speciesCallCount;
                return Promise;
            },
        });

        try {
            Promise.all([Promise.resolve(42)]);
            expect(speciesCallCount).toBe(1);
            runQueuedPromiseJobs();
            expect(speciesCallCount).toBe(1);
        } finally {
            Object.defineProperty(Promise, Symbol.species, originalSpecies);
        }
    });

    test("constructs a custom Symbol.species promise when invoking then", () => {
        const originalSpecies = Object.getOwnPropertyDescriptor(Promise, Symbol.species);
        let constructionCount = 0;

        class SpeciesPromise extends Promise {
            constructor(executor) {
                super(executor);
                ++constructionCount;
            }
        }

        Object.defineProperty(Promise, Symbol.species, {
            configurable: true,
            value: SpeciesPromise,
        });

        try {
            Promise.all([Promise.resolve(42)]);
            expect(constructionCount).toBe(1);
            runQueuedPromiseJobs();
            expect(constructionCount).toBe(1);
        } finally {
            Object.defineProperty(Promise, Symbol.species, originalSpecies);
        }
    });
});

describe("exceptional behavior", () => {
    test("cannot invoke capabilities executor twice", () => {
        function fn() {}

        expect(() => {
            function promise(executor) {
                executor(fn, fn);
                executor(fn, fn);
            }

            Promise.all.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(fn, undefined);
                executor(fn, fn);
            }

            Promise.all.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(undefined, fn);
                executor(fn, fn);
            }

            Promise.all.call(promise, []);
        }).toThrow(TypeError);
    });

    test("promise without resolve method", () => {
        expect(() => {
            function promise(executor) {}
            Promise.all.call(promise, []);
        }).toThrow(TypeError);
    });

    test("no parameters", () => {
        let rejectionReason = null;
        Promise.all().catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });

    test("non-iterable", () => {
        let rejectionReason = null;
        Promise.all(1).catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });
});
