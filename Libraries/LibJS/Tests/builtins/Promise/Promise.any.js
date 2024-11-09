test("length is 1", () => {
    expect(Promise.any).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise", () => {
        const promise = Promise.any();
        expect(promise).toBeInstanceOf(Promise);
    });

    test("all resolve", () => {
        const promise1 = Promise.resolve(3);
        const promise2 = 42;
        const promise3 = new Promise((resolve, reject) => {
            resolve("foo");
        });

        let resolvedValues = null;
        let wasRejected = false;

        Promise.any([promise1, promise2, promise3]).then(
            values => {
                resolvedValues = values;
            },
            () => {
                wasRejected = true;
            }
        );

        runQueuedPromiseJobs();
        expect(resolvedValues).toBe(3);
        expect(wasRejected).toBeFalse();
    });

    test("last resolve", () => {
        const promise1 = Promise.reject(3);
        const promise2 = new Promise((resolve, reject) => {
            resolve("foo");
        });

        let resolvedValues = null;
        let wasRejected = false;

        Promise.any([promise1, promise2]).then(
            values => {
                resolvedValues = values;
            },
            () => {
                wasRejected = true;
            }
        );

        runQueuedPromiseJobs();
        expect(resolvedValues).toBe("foo");
        expect(wasRejected).toBeFalse();
    });

    test("reject", () => {
        const promise1 = Promise.reject(3);
        const promise2 = new Promise((resolve, reject) => {
            reject("foo");
        });

        let rejectionReason = null;
        let wasResolved = false;

        Promise.any([promise1, promise2]).then(
            () => {
                wasResolved = true;
            },
            reason => {
                rejectionReason = reason;
            }
        );

        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(AggregateError);
        expect(wasResolved).toBeFalse();
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

            Promise.any.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(fn, undefined);
                executor(fn, fn);
            }

            Promise.any.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(undefined, fn);
                executor(fn, fn);
            }

            Promise.any.call(promise, []);
        }).toThrow(TypeError);
    });

    test("promise without resolve method", () => {
        expect(() => {
            function promise(executor) {}
            Promise.any.call(promise, []);
        }).toThrow(TypeError);
    });

    test("no parameters", () => {
        let rejectionReason = null;
        Promise.any().catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });

    test("non-iterable", () => {
        let rejectionReason = null;
        Promise.any(1).catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });

    test("empty list", () => {
        let rejectionReason = null;
        Promise.any([]).catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(AggregateError);
    });
});
