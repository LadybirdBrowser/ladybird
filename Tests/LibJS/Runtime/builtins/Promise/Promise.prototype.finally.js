test("length is 1", () => {
    expect(Promise.prototype.finally).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise object different from the initial Promise", () => {
        const initialPromise = new Promise(() => {});
        const finallyPromise = initialPromise.finally();
        expect(finallyPromise).toBeInstanceOf(Promise);
        expect(initialPromise).not.toBe(finallyPromise);
    });

    test("finally() onFinally handler is called when Promise is resolved", () => {
        let resolvePromise = null;
        let finallyWasCalled = false;
        new Promise(resolve => {
            resolvePromise = resolve;
        }).finally(() => {
            finallyWasCalled = true;
        });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(finallyWasCalled).toBeTrue();
    });

    test("finally() onFinally handler is called when Promise is rejected", () => {
        let rejectPromise = null;
        let finallyWasCalled = false;
        new Promise((_, reject) => {
            rejectPromise = reject;
        }).finally(() => {
            finallyWasCalled = true;
        });
        rejectPromise();
        runQueuedPromiseJobs();
        expect(finallyWasCalled).toBeTrue();
    });

    test("works with any object", () => {
        let thenFinallyArg = null;
        let catchFinallyArg = null;
        const onFinally = () => {};
        const thenable = {
            then: (thenFinally, catchFinally) => {
                thenFinallyArg = thenFinally;
                catchFinallyArg = catchFinally;
            },
        };
        Promise.prototype.finally.call(thenable, onFinally);
        expect(typeof thenFinallyArg).toBe("function");
        expect(typeof catchFinallyArg).toBe("function");
        expect(thenFinallyArg).not.toBe(catchFinallyArg);
    });
});

describe("errors", () => {
    test("this value must be an object", () => {
        expect(() => {
            Promise.prototype.finally.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
    });
});
