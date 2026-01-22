describe("normal behavior", () => {
    test("length is 2", () => {
        expect(ShadowRealm.prototype.importValue).toHaveLength(2);
    });

    test("fails if module cannot be loaded", () => {
        // NOTE: The actual import is currently not implemented and always pretends to fail for now.
        const shadowRealm = new ShadowRealm();
        const promise = shadowRealm.importValue("./file_should_not_exist.js", "foo");
        let error;
        promise.catch(value => {
            error = value;
        });
        expect(promise).toBeInstanceOf(Promise);
        runQueuedPromiseJobs();
        expect(error).toBeInstanceOf(TypeError);
        expect(error.message).toBe("Cannot find/open module: './file_should_not_exist.js'");
    });

    test("basic functionality", () => {
        const shadowRealm = new ShadowRealm();
        const promise = shadowRealm.importValue("./external-module.mjs", "foo");
        expect(promise).toBeInstanceOf(Promise);
        let error = null;
        let passed = false;
        promise
            .then(value => {
                expect(value).toBe("Well hello shadows");
                expect(typeof value).toBe("string");

                expect(value).not.toHaveProperty("default", null);
                expect(value).not.toHaveProperty("bar", null);
                passed = true;
            })
            .catch(value => {
                error = value;
            });
        runQueuedPromiseJobs();
        expect(error).toBeNull();
        expect(passed).toBeTrue();
    });

    test("value from async module", () => {
        const shadowRealm = new ShadowRealm();
        const promise = shadowRealm.importValue("./async-module.mjs", "foo");
        expect(promise).toBeInstanceOf(Promise);
        let error = null;
        let passed = false;
        promise
            .then(value => {
                expect(value).toBe("Well hello async shadows");
                expect(typeof value).toBe("string");

                expect(value).not.toHaveProperty("default", null);
                expect(value).not.toHaveProperty("bar", null);
                expect(value).not.toHaveProperty("baz", null);
                expect(value).not.toHaveProperty("qux", null);
                passed = true;
            })
            .catch(value => {
                error = value;
            });
        runQueuedPromiseJobs();
        expect(error).toBeNull();
        expect(passed).toBeTrue();
    });

    test("value from async module from top-level awaited function", () => {
        const shadowRealm = new ShadowRealm();
        const promise = shadowRealm.importValue("./async-module.mjs", "qux");
        expect(promise).toBeInstanceOf(Promise);
        let error = null;
        let passed = false;
        promise
            .then(value => {
                expect(value).toBe("'qux' export");
                expect(typeof value).toBe("string");

                expect(value).not.toHaveProperty("default", null);
                expect(value).not.toHaveProperty("foo", null);
                expect(value).not.toHaveProperty("bar", null);
                expect(value).not.toHaveProperty("baz", null);
                passed = true;
            })
            .catch(value => {
                error = value;
            });
        runQueuedPromiseJobs();
        expect(error).toBeNull();
        expect(passed).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a ShadowRealm object", () => {
        expect(() => {
            ShadowRealm.prototype.importValue.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type ShadowRealm");
    });

    test("export name must be string", () => {
        const shadowRealm = new ShadowRealm();
        expect(() => {
            shadowRealm.importValue("./whatever.mjs", 123);
        }).toThrowWithMessage(TypeError, "123 is not a string");
    });
});
