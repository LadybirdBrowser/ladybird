test("length is 1", () => {
    expect(AsyncDisposableStack.prototype.defer).toHaveLength(1);
});

describe("basic functionality", () => {
    test("deferred function gets called when stack is disposed", async () => {
        const stack = new AsyncDisposableStack();
        let disposedCalled = 0;
        expect(disposedCalled).toBe(0);
        const result = stack.defer((...args) => {
            expect(args.length).toBe(0);
            ++disposedCalled;
        });
        expect(result).toBeUndefined();

        expect(disposedCalled).toBe(0);
        await stack.disposeAsync();
        expect(disposedCalled).toBe(1);
        await stack.disposeAsync();
        expect(disposedCalled).toBe(1);
    });

    test("deferred stack is already disposed", async () => {
        const stack = new AsyncDisposableStack();
        stack.defer(() => {
            expect(stack.disposed).toBeTrue();
        });
        await stack.disposeAsync();
    });
});

describe("throws errors", () => {
    test("if call back is not a function throws type error", () => {
        const stack = new AsyncDisposableStack();
        [
            1,
            1n,
            "a",
            Symbol.dispose,
            NaN,
            0,
            {},
            [],
            { f() {} },
            { [Symbol.dispose]() {} },
            {
                get [Symbol.dispose]() {
                    return () => {};
                },
            },
        ].forEach(value => {
            expect(() => stack.defer(value)).toThrowWithMessage(TypeError, "not a function");
        });

        expect(stack.disposed).toBeFalse();
    });

    test("defer throws if stack is already disposed (over type errors)", async () => {
        const stack = new AsyncDisposableStack();
        await stack.disposeAsync();
        expect(stack.disposed).toBeTrue();

        [{ [Symbol.dispose]() {} }, 1, null, undefined, "a", []].forEach(value => {
            expect(() => stack.defer(value)).toThrowWithMessage(
                ReferenceError,
                "AsyncDisposableStack already disposed values"
            );
        });
    });
});
