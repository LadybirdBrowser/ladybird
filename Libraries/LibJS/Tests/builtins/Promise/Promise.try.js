describe("errors", () => {
    test("this value must be a constructor", () => {
        expect(() => {
            Promise.try.call({});
        }).toThrowWithMessage(TypeError, "[object Object] is not a constructor");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Promise.try).toHaveLength(1);
    });

    test("returned promise is a Promise", () => {
        const fn = () => {};
        const promise = Promise.try(fn);
        expect(promise).toBeInstanceOf(Promise);
    });

    test("returned promise is resolved when function completes normally", () => {
        const fn = () => {};
        const promise = Promise.try(fn);

        let fulfillmentValue = null;
        promise.then(value => {
            fulfillmentValue = value;
        });

        runQueuedPromiseJobs();

        expect(fulfillmentValue).toBe(undefined);
    });

    test("returned promise is rejected when function throws", () => {
        const fn = () => {
            throw "error";
        };
        const promise = Promise.try(fn);

        let rejectionReason = null;
        promise.catch(value => {
            rejectionReason = value;
        });

        runQueuedPromiseJobs();

        expect(rejectionReason).toBe("error");
    });

    test("arguments are forwarded to the function", () => {
        const fn = (...args) => args;
        const promise = Promise.try(fn, "foo", 123, true);

        let fulfillmentValue = null;
        promise.then(value => {
            fulfillmentValue = value;
        });

        runQueuedPromiseJobs();

        expect(fulfillmentValue).toEqual(["foo", 123, true]);
    });
});
