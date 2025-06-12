test("length is 1", () => {
    expect(Promise.resolve).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns a Promise", () => {
        const resolvedPromise = Promise.resolve();
        expect(resolvedPromise).toBeInstanceOf(Promise);
    });

    test("returned Promise is resolved with given argument", () => {
        let fulfillmentValue = null;
        Promise.resolve("Some value").then(value => {
            fulfillmentValue = value;
        });
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });

    test("works with subclasses", () => {
        class CustomPromise extends Promise {}

        const resolvedPromise = CustomPromise.resolve("Some value");
        expect(resolvedPromise).toBeInstanceOf(CustomPromise);

        let fulfillmentValue = null;
        resolvedPromise.then(value => {
            fulfillmentValue = value;
        });
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });
});

describe("errors", () => {
    test("this value must be an object", () => {
        expect(() => {
            Promise.resolve.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
    });
});
