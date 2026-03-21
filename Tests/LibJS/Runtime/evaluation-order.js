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

test("object literal key is reassigned during value evaluation", () => {
    function foo(key, value) {
        let object = { [key]: (key = value) };
        return Object.keys(object)[0];
    }
    let result = foo("old", "new");
    expect(result).toBe("old");
});

test("evaluation order for compound assignment", () => {
    function foo(value) {
        return (value += value = 5);
    }
    let result = foo(2);
    expect(result).toBe(7);
});

test("evaluation order for binary operators (RHS reassigns)", () => {
    function foo(value) {
        return value + (value = 5);
    }
    let result = foo(2);
    expect(result).toBe(7);
});

test("evaluation order for binary operators (LHS reassigns)", () => {
    function foo(value) {
        return (value = 5) + value;
    }
    let result = foo(2);
    expect(result).toBe(10);
});

test("evaluation order for member expression", () => {
    function foo(obj, key) {
        return obj[(obj = key)];
    }
    let result = foo({ asdf: 42 }, "asdf");
    expect(result).toBe(42);
});
