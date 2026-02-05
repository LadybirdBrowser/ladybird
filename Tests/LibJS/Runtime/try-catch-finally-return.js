test("return from try block", () => {
    function foo() {
        try {
            return "foo";
        } catch {
            return "bar";
        }
    }
    expect(foo()).toBe("foo");
});

test("return from catch block", () => {
    function foo() {
        try {
            throw "foo";
        } catch {
            return "bar";
        }
    }
    expect(foo()).toBe("bar");
});

test("return from finally block", () => {
    function foo() {
        try {
            return "foo";
        } catch {
            return "bar";
        } finally {
            return "baz";
        }
    }
    expect(foo()).toBe("baz");
});

test("return from catch block with empty finally", () => {
    function foo() {
        try {
            throw "foo";
        } catch {
            return "bar";
        } finally {
        }
    }
    expect(foo()).toBe("bar");
});
