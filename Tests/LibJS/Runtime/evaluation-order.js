test("Assignment should always evaluate LHS first", () => {
    function go(a) {
        let i = 0;
        a[i] = a[++i];
    }

    let a = [1, 2, 3];
    go(a);
    expect(a).toEqual([2, 2, 3]);
});

test("Binary assignment should always evaluate LHS first", () => {
    function go(a) {
        let i = 0;
        a[i] |= a[++i];
    }

    let a = [1, 2];
    go(a);
    expect(a).toEqual([3, 2]);
});

test("Base object of lhs of assignment is copied to preserve evaluation order", () => {
    let topLevel = {};
    function go() {
        let temp = topLevel;
        temp.test = temp = temp.test || {};
    }

    go();
    expect(topLevel.test).not.toBeUndefined();
    expect(topLevel.test).toEqual({});
});

test("optional chain call preserves argument evaluation order", () => {
    let a = 1;
    let fn = x => x;
    let result = fn?.(a, (a = 42));
    expect(result).toBe(1);
    expect(a).toBe(42);
});

test("optional chain call with multiple arguments preserving order", () => {
    let x = 10;
    let fn = (a, b) => a + b;
    let result = fn?.(x, ((x = 20), 5));
    expect(result).toBe(15);
    expect(x).toBe(20);
});

test("postfix increment evaluation order", () => {
    function bar(a, b) {
        expect(a).toBe(0);
        expect(b).toBe(0);
    }

    function foo() {
        let i = 0;
        bar(i, i++);
        expect(i).toBe(1);
    }
    foo();
});

test("callee gets reassigned while evaluating arguments", () => {
    function foo(func, value) {
        return func((func = value));
    }

    let result = foo(x => x, 42);
    expect(result).toBe(42);
});
