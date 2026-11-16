test("parameter expressions that capture a parameter keep their own variable environment", () => {
    function f(a, read = () => a) {
        var a = 2;
        return [read(), a];
    }
    for (let i = 0; i < 100; ++i) expect(f(1)).toEqual([1, 2]);
});

test("functions in parameters see the parameters, the body sees its variables", () => {
    function f(
        x,
        g = function () {
            return x;
        }
    ) {
        var x;
        var y = g();
        x = 5;
        return [y, g(), x];
    }
    for (let i = 0; i < 100; ++i) expect(f(3)).toEqual([3, 3, 5]);
});

test("sloppy direct eval declares variables in the function", () => {
    function f(code) {
        eval(code);
        return typeof declared === "undefined" ? "none" : declared;
    }
    for (let i = 0; i < 50; ++i) {
        expect(f("var declared = 'yes'")).toBe("yes");
        expect(f("let declared = 'no'")).toBe("none");
    }
});

test("sloppy direct eval in parameter expressions", () => {
    function f(a = eval("var b = 7; b"), c = () => b) {
        return [a, c()];
    }
    for (let i = 0; i < 50; ++i) expect(f()).toEqual([7, 7]);
});

test("block functions in sloppy functions set their variable binding", () => {
    function f() {
        {
            function inner() {
                return "inner";
            }
        }
        return inner();
    }
    function g(x = 1) {
        {
            function inner() {
                return x;
            }
        }
        return inner();
    }
    for (let i = 0; i < 100; ++i) {
        expect(f()).toBe("inner");
        expect(g()).toBe(1);
    }
});

test("generators and async functions with parameter expressions", async () => {
    function* g(a, read = () => a) {
        var a = 2;
        yield read();
        yield a;
    }
    async function h(a, read = () => a) {
        var a = 4;
        await null;
        return [read(), a];
    }
    for (let i = 0; i < 50; ++i) expect([...g(1)]).toEqual([1, 2]);
    expect(await h(3)).toEqual([3, 4]);
});

test("var bindings of closures in a hot function", () => {
    function counter() {
        var count = 0;
        return () => ++count;
    }
    const next = counter();
    let last = 0;
    for (let i = 0; i < 1000; ++i) last = next();
    expect(last).toBe(1000);
});
