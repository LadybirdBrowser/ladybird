test("length is 0", () => {
    expect(DisposableStack.prototype[Symbol.dispose]).toHaveLength(0);
});

test("is the same as dispose", () => {
    expect(DisposableStack.prototype[Symbol.dispose]).toBe(DisposableStack.prototype.dispose);
});

describe("used in using functionality", () => {
    test.xfail("make the stack marked as disposed", () => {
        let innerStack;
        {
            using stack = new DisposableStack();
            innerStack = stack;
            expect(stack.disposed).toBeFalse();
        }
        expect(innerStack.disposed).toBeTrue();
    });
});
