test("length is 1", () => {
    expect(DisposableStack.prototype.defer).toHaveLength(1);
});

describe("basic functionality", () => {
    test("deferred function gets called when stack is disposed", () => {
        const stack = new DisposableStack();
        let disposedCalled = 0;
        expect(disposedCalled).toBe(0);
        const result = stack.defer((...args) => {
            expect(args.length).toBe(0);
            ++disposedCalled;
        });
        expect(result).toBeUndefined();

        expect(disposedCalled).toBe(0);
        stack.dispose();
        expect(disposedCalled).toBe(1);
        stack.dispose();
        expect(disposedCalled).toBe(1);
    });

    test("deferred stack is already disposed", () => {
        const stack = new DisposableStack();
        stack.defer(() => {
            expect(stack.disposed).toBeTrue();
        });
        stack.dispose();
    });
});

describe("throws errors", () => {
    test("if call back is not a function throws type error", () => {
        const stack = new DisposableStack();
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

    test("defer throws if stack is already disposed (over type errors)", () => {
        const stack = new DisposableStack();
        stack.dispose();
        expect(stack.disposed).toBeTrue();

        [{ [Symbol.dispose]() {} }, 1, null, undefined, "a", []].forEach(value => {
            expect(() => stack.defer(value)).toThrowWithMessage(
                ReferenceError,
                "DisposableStack already disposed values"
            );
        });
    });
});
