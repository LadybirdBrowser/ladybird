test("length is 1", () => {
    expect(Promise.allSettled).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise", () => {
        const promise = Promise.allSettled();
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

        Promise.allSettled([promise1, promise2, promise3]).then(
            values => {
                resolvedValues = values;
            },
            () => {
                wasRejected = true;
            }
        );

        runQueuedPromiseJobs();
        expect(resolvedValues).toEqual([
            { status: "fulfilled", value: 3 },
            { status: "fulfilled", value: 42 },
            { status: "fulfilled", value: "foo" },
        ]);
        expect(wasRejected).toBeFalse();
    });

    test("reject", () => {
        const promise1 = Promise.resolve(3);
        const promise2 = 42;
        const promise3 = new Promise((resolve, reject) => {
            reject("foo");
        });

        let resolvedValues = null;
        let wasRejected = false;

        Promise.allSettled([promise1, promise2, promise3]).then(
            values => {
                resolvedValues = values;
            },
            () => {
                wasRejected = true;
            }
        );

        runQueuedPromiseJobs();
        expect(resolvedValues).toEqual([
            { status: "fulfilled", value: 3 },
            { status: "fulfilled", value: 42 },
            { status: "rejected", reason: "foo" },
        ]);
        expect(wasRejected).toBeFalse();
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

            Promise.allSettled.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(fn, undefined);
                executor(fn, fn);
            }

            Promise.allSettled.call(promise, []);
        }).toThrow(TypeError);

        expect(() => {
            function promise(executor) {
                executor(undefined, fn);
                executor(fn, fn);
            }

            Promise.allSettled.call(promise, []);
        }).toThrow(TypeError);
    });

    test("promise without resolve method", () => {
        expect(() => {
            function promise(executor) {}
            Promise.allSettled.call(promise, []);
        }).toThrow(TypeError);
    });

    test("no parameters", () => {
        let rejectionReason = null;
        Promise.allSettled().catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });

    test("non-iterable", () => {
        let rejectionReason = null;
        Promise.allSettled(1).catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeInstanceOf(TypeError);
    });
});
