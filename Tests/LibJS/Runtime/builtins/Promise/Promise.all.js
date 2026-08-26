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

    test("invokes a then getter that returns the original method", () => {
        const promise = Promise.resolve(42);
        const originalThen = promise.then;
        let callCount = 0;
        let resolvedValues = null;

        Object.defineProperty(promise, "then", {
            configurable: true,
            get() {
                ++callCount;
                return originalThen;
            },
        });

        Promise.all([promise]).then(values => {
            resolvedValues = values;
        });

        runQueuedPromiseJobs();
        expect(callCount).toBe(1);
        expect(resolvedValues).toEqual([42]);
    });

    test("observes the promise constructor when invoking then", () => {
        const promise = Promise.resolve(42);
        let constructorCallCount = 0;

        Object.defineProperty(promise, "constructor", {
            configurable: true,
            get() {
                ++constructorCallCount;
                return Promise;
            },
        });

        Promise.all([promise]);
        expect(constructorCallCount).toBe(2);
        runQueuedPromiseJobs();
        expect(constructorCallCount).toBe(2);
    });

    test("resolves the result array through an inherited then getter", () => {
        const originalThen = Object.getOwnPropertyDescriptor(Array.prototype, "then");
        const marker = {};
        let thenCallCount = 0;
        let resolvedValues = null;

        Object.defineProperty(Array.prototype, "then", {
            configurable: true,
            get() {
                if (this.length === 1 && this[0] === marker) ++thenCallCount;
                return undefined;
            },
        });

        try {
            Promise.all([Promise.resolve(marker)]).then(values => {
                resolvedValues = values;
            });

            runQueuedPromiseJobs();
        } finally {
            if (originalThen) Object.defineProperty(Array.prototype, "then", originalThen);
            else delete Array.prototype.then;
        }

        expect(thenCallCount).toBe(1);
        expect(resolvedValues).toEqual([marker]);
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
