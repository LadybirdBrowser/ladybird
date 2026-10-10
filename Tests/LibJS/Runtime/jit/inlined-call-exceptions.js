// An exception thrown by a call that an inlined callee makes, caught by a handler in the function the callee is
// inlined into, which then reads what it set before the call.

test("exceptions from calls in inlined callees reach handlers that read the frame", () => {
    function thrower() {
        const notAFunction = undefined;
        return notAFunction(...arguments);
    }
    const target = () => thrower(1);
    function run() {
        let threw = true;
        let result = null;
        try {
            target();
            threw = false;
        } catch (error) {
            result = error instanceof TypeError ? "TypeError" : `other ${error}`;
        }
        if (!threw) result = "none";
        return result;
    }
    for (let i = 0; i < 200; ++i) expect(run()).toBe("TypeError");
});
