test("length is 2", () => {
    expect(DisposableStack.prototype.adopt).toHaveLength(2);
});

describe("basic functionality", () => {
    test("adopted dispose method gets called when stack is disposed", () => {
        const stack = new DisposableStack();
        let disposedCalled = 0;
        let disposeArgument = undefined;
        expect(disposedCalled).toBe(0);
        const result = stack.adopt(null, arg => {
            disposeArgument = arg;
            ++disposedCalled;
        });
        expect(result).toBeNull();

        expect(disposedCalled).toBe(0);
        stack.dispose();
        expect(disposedCalled).toBe(1);
        expect(disposeArgument).toBeNull();
        stack.dispose();
        expect(disposedCalled).toBe(1);
    });

    test("can adopt any value", () => {
        const stack = new DisposableStack();
        const disposed = [];
        function dispose(value) {
            disposed.push(value);
        }

        const values = [null, undefined, 1, "a", Symbol.dispose, () => {}, new WeakMap(), [], {}];

        values.forEach(value => {
            stack.adopt(value, dispose);
        });

        stack.dispose();

        expect(disposed).toEqual(values.reverse());
    });

    test("adopted stack is already disposed", () => {
        const stack = new DisposableStack();
        stack.adopt(stack, value => {
            expect(stack).toBe(value);
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
            expect(() => stack.adopt(null, value)).toThrowWithMessage(TypeError, "not a function");
        });

        expect(stack.disposed).toBeFalse();
    });

    test("adopt throws if stack is already disposed (over type errors)", () => {
        const stack = new DisposableStack();
        stack.dispose();
        expect(stack.disposed).toBeTrue();

        [{ [Symbol.dispose]() {} }, 1, null, undefined, "a", []].forEach(value => {
            expect(() => stack.adopt(value, () => {})).toThrowWithMessage(
                ReferenceError,
                "DisposableStack already disposed values"
            );
            expect(() => stack.adopt(null, value)).toThrowWithMessage(
                ReferenceError,
                "DisposableStack already disposed values"
            );
        });
    });
});
