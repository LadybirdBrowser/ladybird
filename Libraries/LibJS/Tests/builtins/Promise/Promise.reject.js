test("length is 1", () => {
    expect(Promise.reject).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise", () => {
        const rejectedPromise = Promise.reject();
        expect(rejectedPromise).toBeInstanceOf(Promise);
    });

    test("returned Promise is rejected with given argument", () => {
        let rejectionReason = null;
        Promise.reject("Some value").catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBe("Some value");
    });

    test("works with subclasses", () => {
        class CustomPromise extends Promise {}

        const rejectedPromise = CustomPromise.reject("Some value");
        expect(rejectedPromise).toBeInstanceOf(CustomPromise);

        let rejectionReason = null;
        rejectedPromise.catch(reason => {
            rejectionReason = reason;
        });
        runQueuedPromiseJobs();
        expect(rejectionReason).toBe("Some value");
    });
});
