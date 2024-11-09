test("length is 1", () => {
    expect(Promise).toHaveLength(1);
});

describe("errors", () => {
    test("must be called as constructor", () => {
        expect(() => {
            Promise();
        }).toThrowWithMessage(TypeError, "Promise constructor must be called with 'new'");
    });

    test("executor must be a function", () => {
        expect(() => {
            new Promise();
        }).toThrowWithMessage(TypeError, "Promise executor must be a function");
    });
});

describe("normal behavior", () => {
    test("returns a Promise object", () => {
        const promise = new Promise(() => {});
        expect(promise).toBeInstanceOf(Promise);
        expect(typeof promise).toBe("object");
    });

    test("executor is called with resolve and reject functions", () => {
        let resolveFunction = null;
        let rejectFunction = null;
        new Promise((resolve, reject) => {
            resolveFunction = resolve;
            rejectFunction = reject;
        });
        expect(typeof resolveFunction).toBe("function");
        expect(typeof rejectFunction).toBe("function");
        expect(resolveFunction).not.toBe(rejectFunction);
    });
});
