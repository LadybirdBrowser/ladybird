test("length is 1", () => {
    expect(DisposableStack.prototype.use).toHaveLength(1);
});

describe("basic functionality", () => {
    test("added objects dispose method gets when stack is disposed", () => {
        const stack = new DisposableStack();
        let disposedCalled = 0;
        const obj = {
            [Symbol.dispose]() {
                ++disposedCalled;
            },
        };
        expect(disposedCalled).toBe(0);
        const result = stack.use(obj);
        expect(result).toBe(obj);

        expect(disposedCalled).toBe(0);
        stack.dispose();
        expect(disposedCalled).toBe(1);
        stack.dispose();
        expect(disposedCalled).toBe(1);
    });

    test("can add null and undefined", () => {
        const stack = new DisposableStack();

        expect(stack.use(null)).toBeNull();
        expect(stack.use(undefined)).toBeUndefined();

        expect(stack.disposed).toBeFalse();
        stack.dispose();
        expect(stack.disposed).toBeTrue();
    });

    test("can add stack to itself", () => {
        const stack = new DisposableStack();
        stack.use(stack);
        stack.dispose();
    });
});

describe("throws errors", () => {
    test("if added value is not an object or null or undefined throws type error", () => {
        const stack = new DisposableStack();
        [1, 1n, "a", Symbol.dispose, NaN, 0].forEach(value => {
            expect(() => stack.use(value)).toThrowWithMessage(TypeError, "not an object");
        });

        expect(stack.disposed).toBeFalse();
    });

    test("if added object does not have a dispose method throws type error", () => {
        const stack = new DisposableStack();
        [{}, [], { f() {} }].forEach(value => {
            expect(() => stack.use(value)).toThrowWithMessage(
                TypeError,
                "does not have dispose method"
            );
        });

        expect(stack.disposed).toBeFalse();
    });

    test("if added object has non function dispose method it throws type error", () => {
        const stack = new DisposableStack();
        let calledGetter = false;
        [
            { [Symbol.dispose]: 1 },
            {
                get [Symbol.dispose]() {
                    calledGetter = true;
                    return 1;
                },
            },
        ].forEach(value => {
            expect(() => stack.use(value)).toThrowWithMessage(TypeError, "is not a function");
        });

        expect(stack.disposed).toBeFalse();
        expect(calledGetter).toBeTrue();
    });

    test("use throws if stack is already disposed (over type errors)", () => {
        const stack = new DisposableStack();
        stack.dispose();
        expect(stack.disposed).toBeTrue();

        [{ [Symbol.dispose]() {} }, 1, null, undefined, "a", []].forEach(value => {
            expect(() => stack.use(value)).toThrowWithMessage(
                ReferenceError,
                "DisposableStack already disposed values"
            );
        });
    });
});
