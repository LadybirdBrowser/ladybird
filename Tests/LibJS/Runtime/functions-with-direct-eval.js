test("a var from eval in a function with parameter expressions is apart from the parameters", () => {
    function f(a = 1) {
        eval("var a = 2");
        return a;
    }
    expect(f()).toBe(2);

    function g({ a = 1 }) {
        eval("var a = 2");
        return a;
    }
    expect(g({})).toBe(2);
});

test("a var from eval clashes with a let of the function", () => {
    function f() {
        let x = 1;
        eval("var x = 2");
    }
    expect(f).toThrow(SyntaxError);
});

test("a var from eval gets in the way of the name for nested functions", () => {
    function f() {
        function read() {
            return addedByEval;
        }
        eval("var addedByEval = 2");
        return read();
    }
    expect(f()).toBe(2);
});

test("eval in a parenthesis is direct", () => {
    function f() {
        const local = 1;
        return eval("local");
    }
    expect(f()).toBe(1);
});

test("duplicate parameters that eval can see", () => {
    function f(a, a) {
        return eval("a");
    }
    expect(f(1, 2)).toBe(2);

    function g(a, a, b) {
        return () => b;
    }
    expect(g(1, 2, 3)()).toBe(3);
});

test("arguments and block functions of that name", () => {
    function f(a) {
        arguments[0] = 9;
        return eval("a");
    }
    expect(f(1)).toBe(9);

    function g() {
        return (() => eval("arguments.length"))();
    }
    expect(g(1, 2)).toBe(2);
});

test("a var from eval next to a let of the function, in calls with different names", () => {
    function f(source) {
        let result;
        result = eval(source);
        return result;
    }
    expect(f("var a = 1; a")).toBe(1);
    expect(f("var b = 2; b")).toBe(2);
});
