describe("errors", () => {
    test("this value must be a constructor", () => {
        expect(() => {
            Promise.withResolvers.call(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a constructor");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Promise.withResolvers).toHaveLength(0);
    });

    test("returned promise is a Promise", () => {
        const { promise, resolve, reject } = Promise.withResolvers();
        expect(promise).toBeInstanceOf(Promise);
    });

    test("returned resolve/reject are unary functions", () => {
        const { promise, resolve, reject } = Promise.withResolvers();

        expect(resolve).toBeInstanceOf(Function);
        expect(resolve).toHaveLength(1);

        expect(reject).toBeInstanceOf(Function);
        expect(reject).toHaveLength(1);
    });

    test("returned promise can be resolved", () => {
        const { promise, resolve, reject } = Promise.withResolvers();

        let fulfillmentValue = null;
        promise.then(value => {
            fulfillmentValue = value;
        });

        resolve("Some value");
        runQueuedPromiseJobs();

        expect(fulfillmentValue).toBe("Some value");
    });

    test("returned promise can be rejected", () => {
        const { promise, resolve, reject } = Promise.withResolvers();

        let rejectionReason = null;
        promise.catch(value => {
            rejectionReason = value;
        });

        reject("Some value");
        runQueuedPromiseJobs();

        expect(rejectionReason).toBe("Some value");
    });
});
