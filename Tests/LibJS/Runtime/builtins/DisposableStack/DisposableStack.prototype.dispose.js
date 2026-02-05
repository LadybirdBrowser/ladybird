test("length is 0", () => {
    expect(DisposableStack.prototype.dispose).toHaveLength(0);
});

describe("basic functionality", () => {
    test("make the stack marked as disposed", () => {
        const stack = new DisposableStack();
        const result = stack.dispose();
        expect(stack.disposed).toBeTrue();
        expect(result).toBeUndefined();
    });

    test("call dispose on objects in stack when called", () => {
        const stack = new DisposableStack();
        let disposedCalled = false;
        stack.use({
            [Symbol.dispose]() {
                disposedCalled = true;
            },
        });

        expect(disposedCalled).toBeFalse();
        const result = stack.dispose();
        expect(disposedCalled).toBeTrue();
        expect(result).toBeUndefined();
    });

    test("disposed the objects added to the stack in reverse order", () => {
        const disposed = [];
        const stack = new DisposableStack();
        stack.use({
            [Symbol.dispose]() {
                disposed.push("a");
            },
        });
        stack.use({
            [Symbol.dispose]() {
                disposed.push("b");
            },
        });

        expect(disposed).toEqual([]);
        const result = stack.dispose();
        expect(disposed).toEqual(["b", "a"]);
        expect(result).toBeUndefined();
    });

    test("does not dispose anything if already disposed", () => {
        const disposed = [];
        const stack = new DisposableStack();
        stack.use({
            [Symbol.dispose]() {
                disposed.push("a");
            },
        });

        expect(stack.disposed).toBeFalse();
        expect(disposed).toEqual([]);

        expect(stack.dispose()).toBeUndefined();

        expect(stack.disposed).toBeTrue();
        expect(disposed).toEqual(["a"]);

        expect(stack.dispose()).toBeUndefined();

        expect(stack.disposed).toBeTrue();
        expect(disposed).toEqual(["a"]);
    });

    test("throws if dispose method throws", () => {
        const stack = new DisposableStack();
        let disposedCalled = false;
        stack.use({
            [Symbol.dispose]() {
                disposedCalled = true;
                expect().fail("fail in dispose");
            },
        });

        expect(() => stack.dispose()).toThrowWithMessage(ExpectationError, "fail in dispose");
    });
});
