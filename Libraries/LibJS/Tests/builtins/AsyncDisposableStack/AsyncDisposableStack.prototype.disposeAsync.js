test("length is 0", () => {
    expect(AsyncDisposableStack.prototype.disposeAsync).toHaveLength(0);
});

describe("basic functionality", () => {
    test("make the stack marked as disposed", async () => {
        const stack = new AsyncDisposableStack();
        const result = await stack.disposeAsync();
        expect(stack.disposed).toBeTrue();
        expect(result).toBeUndefined();
    });

    test("call dispose on objects in stack when called", async () => {
        const stack = new AsyncDisposableStack();
        let disposedCalled = false;
        stack.use({
            [Symbol.asyncDispose]() {
                disposedCalled = true;
            },
        });

        expect(disposedCalled).toBeFalse();
        const result = await stack.disposeAsync();
        expect(disposedCalled).toBeTrue();
        expect(result).toBeUndefined();
    });

    test("disposed the objects added to the stack in reverse order", async () => {
        const disposed = [];
        const stack = new AsyncDisposableStack();
        stack.use({
            [Symbol.asyncDispose]() {
                disposed.push("a");
            },
        });
        stack.use({
            [Symbol.asyncDispose]() {
                disposed.push("b");
            },
        });

        expect(disposed).toEqual([]);
        const result = await stack.disposeAsync();
        expect(disposed).toEqual(["b", "a"]);
        expect(result).toBeUndefined();
    });

    test("does not dispose anything if already disposed", async () => {
        const disposed = [];
        const stack = new AsyncDisposableStack();
        stack.use({
            [Symbol.asyncDispose]() {
                disposed.push("a");
            },
        });

        expect(stack.disposed).toBeFalse();
        expect(disposed).toEqual([]);

        let result = await stack.disposeAsync();
        expect(result).toBeUndefined();

        expect(stack.disposed).toBeTrue();
        expect(disposed).toEqual(["a"]);

        result = await stack.disposeAsync();
        expect(result).toBeUndefined();

        expect(stack.disposed).toBeTrue();
        expect(disposed).toEqual(["a"]);
    });

    test("throws if dispose method throws", async () => {
        const stack = new AsyncDisposableStack();
        let disposedCalled = false;
        stack.use({
            [Symbol.asyncDispose]() {
                disposedCalled = true;
                expect().fail("fail in dispose");
            },
        });

        expect(async () => await stack.disposeAsync()).toThrowWithMessage(ExpectationError, "fail in dispose");
    });
});
