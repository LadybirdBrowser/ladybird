test("length is 2", () => {
    expect(Promise.prototype.then).toHaveLength(2);
});

describe("errors", () => {
    test("this value must be a Promise", () => {
        expect(() => {
            Promise.prototype.then.call({});
        }).toThrowWithMessage(TypeError, "Not an object of type Promise");
    });
});

describe("normal behavior", () => {
    test("returns a Promise object different from the initial Promise", () => {
        const initialPromise = new Promise(() => {});
        const thenPromise = initialPromise.then();
        expect(thenPromise).toBeInstanceOf(Promise);
        expect(initialPromise).not.toBe(thenPromise);
    });

    test("then() onFulfilled handler is called when Promise is fulfilled", () => {
        let resolvePromise = null;
        let fulfillmentValue = null;
        new Promise(resolve => {
            resolvePromise = resolve;
        }).then(
            value => {
                fulfillmentValue = value;
            },
            () => {
                expect().fail();
            }
        );
        resolvePromise("Some value");
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });

    test("then() onRejected handler is called when Promise is rejected", () => {
        let rejectPromise = null;
        let rejectionReason = null;
        new Promise((_, reject) => {
            rejectPromise = reject;
        }).then(
            () => {
                expect().fail();
            },
            reason => {
                rejectionReason = reason;
            }
        );
        rejectPromise("Some reason");
        runQueuedPromiseJobs();
        expect(rejectionReason).toBe("Some reason");
    });

    test("returned Promise is resolved with undefined if handler is missing", () => {
        let resolvePromise = null;
        let fulfillmentValue = null;
        new Promise(resolve => {
            resolvePromise = resolve;
        })
            .then()
            .then(value => {
                fulfillmentValue = value;
            });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBeUndefined();
    });

    test("returned Promise is resolved with return value if handler returns value", () => {
        let resolvePromise = null;
        let fulfillmentValue = null;
        new Promise(resolve => {
            resolvePromise = resolve;
        })
            .then(() => "Some value")
            .then(value => {
                fulfillmentValue = value;
            });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });

    test("returned Promise is rejected with error if handler throws error", () => {
        let resolvePromise = null;
        let rejectionReason = null;
        const error = new Error();
        new Promise(resolve => {
            resolvePromise = resolve;
        })
            .then(() => {
                throw error;
            })
            .catch(reason => {
                rejectionReason = reason;
            });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(rejectionReason).toBe(error);
    });

    test("returned Promise is resolved with Promise result if handler returns fulfilled Promise", () => {
        let resolvePromise = null;
        let fulfillmentValue = null;
        new Promise(resolve => {
            resolvePromise = resolve;
        })
            .then(() => {
                return Promise.resolve("Some value");
            })
            .then(value => {
                fulfillmentValue = value;
            });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });

    test("returned Promise is resolved with thenable result if handler returns thenable", () => {
        let resolvePromise = null;
        let fulfillmentValue = null;
        const thenable = {
            then: onFulfilled => {
                onFulfilled("Some value");
            },
        };
        new Promise(resolve => {
            resolvePromise = resolve;
        })
            .then(() => {
                return thenable;
            })
            .then(value => {
                fulfillmentValue = value;
            });
        resolvePromise();
        runQueuedPromiseJobs();
        expect(fulfillmentValue).toBe("Some value");
    });

    test("PromiseCapability with throwing resolve function", () => {
        class PromiseLike {
            constructor(executor) {
                executor(
                    () => {
                        throw new Error();
                    },
                    () => {}
                );
            }
        }

        let resolvePromise;
        const p = new Promise(resolve => {
            resolvePromise = resolve;
        });
        p.constructor = { [Symbol.species]: PromiseLike };

        // Triggers creation of a PromiseCapability with the throwing resolve function passed to the executor above
        p.then(() => {});

        // Crashes when assuming there are no job exceptions (as per the spec)
        // If we survive this, the exception has been silently ignored
        resolvePromise();
        runQueuedPromiseJobs();
    });
});
