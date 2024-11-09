test("length is 1", () => {
    expect(Promise.prototype.catch).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise object different from the initial Promise", () => {
        const initialPromise = new Promise(() => {});
        const catchPromise = initialPromise.catch();
        expect(catchPromise).toBeInstanceOf(Promise);
        expect(initialPromise).not.toBe(catchPromise);
    });

    test("catch() onRejected handler is called when Promise is rejected", () => {
        let rejectPromise = null;
        let rejectionReason = null;
        new Promise((_, reject) => {
            rejectPromise = reject;
        }).catch(reason => {
            rejectionReason = reason;
        });
        rejectPromise("Some reason");
        runQueuedPromiseJobs();
        expect(rejectionReason).toBe("Some reason");
    });

    test("returned Promise is rejected with undefined if handler is missing", () => {
        let rejectPromise = null;
        let rejectionReason = null;
        new Promise((_, reject) => {
            rejectPromise = reject;
        })
            .catch()
            .catch(reason => {
                rejectionReason = reason;
            });
        rejectPromise();
        runQueuedPromiseJobs();
        expect(rejectionReason).toBeUndefined();
    });

    test("works with any object", () => {
        let onFulfilledArg = null;
        let onRejectedArg = null;
        const onRejected = () => {};
        const thenable = {
            then: (onFulfilled, onRejected) => {
                onFulfilledArg = onFulfilled;
                onRejectedArg = onRejected;
            },
        };
        Promise.prototype.catch.call(thenable, onRejected);
        expect(onFulfilledArg).toBeUndefined();
        expect(onRejectedArg).toBe(onRejected);
    });
});
