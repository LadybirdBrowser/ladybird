var outer = "outer";

test("a function in the parameters does not see the bindings of the body", () => {
    function f(a = () => outer) {
        var outer = 1;
        const read = () => outer;
        return [a(), read()];
    }
    expect(f()).toEqual(["outer", 1]);

    function g(a = () => outer) {
        function outer() {}
        return [a(), typeof outer];
    }
    expect(g()).toEqual(["outer", "function"]);

    function h(a = () => outer) {
        let outer = 1;
        const read = () => outer;
        return [a(), read()];
    }
    expect(h()).toEqual(["outer", 1]);
});

test("a var of the name of a parameter starts with its value and is a binding of its own", () => {
    function f(x, read = () => x) {
        var x;
        const before = x;
        x = 2;
        return [before, read(), x];
    }
    expect(f(1)).toEqual([1, 1, 2]);

    function g(x, write = () => (x = 7)) {
        var x;
        write();
        return x;
    }
    expect(g(1)).toBe(1);

    function h(x = 1) {
        var x;
        const read = () => x;
        x = 2;
        return read();
    }
    expect(h()).toBe(2);

    function i(x, read = () => x) {
        try {
            throw 1;
        } catch (x) {
            var x = 6;
        }
        return [read(), x];
    }
    expect(i(1)).toEqual([1, 1]);
});

test("a var of the name of a parameter inside a scope that has an environment", () => {
    function inBlock(p = "parameter", read = () => p) {
        {
            let q = "block";
            const keep = () => q;
            var p;
        }
        return [p, read()];
    }
    expect(inBlock()).toEqual(["parameter", "parameter"]);

    function inNestedBlocks(p = "parameter", read = () => p) {
        {
            let q = "block";
            const keepQ = () => q;
            {
                let s = "inner block";
                const keepS = () => s;
                var p;
            }
        }
        return p;
    }
    expect(inNestedBlocks()).toBe("parameter");

    function inCatch(p = "parameter", read = () => p) {
        try {
            throw 1;
        } catch (e) {
            const keep = () => e;
            var p;
        }
        return p;
    }
    expect(inCatch()).toBe("parameter");

    function inLoop(p = "parameter", read = () => p) {
        for (let i = 0; i < 1; i++) {
            const keep = () => i;
            var p;
        }
        return p;
    }
    expect(inLoop()).toBe("parameter");

    function inSwitch(p = "parameter", read = () => p) {
        switch (0) {
            case 0:
                let q = "case";
                const keep = () => q;
                var p;
        }
        return p;
    }
    expect(inSwitch()).toBe("parameter");

    function inClassScope(p = "parameter", read = () => p) {
        {
            class K {}
            const keep = () => K;
            var p;
        }
        return p;
    }
    expect(inClassScope()).toBe("parameter");

    function withDirectEval(p = "parameter", read = () => p) {
        {
            let q = "block";
            const keep = () => q;
            var p;
        }
        return eval("p");
    }
    expect(withDirectEval()).toBe("parameter");
});

test("a function declaration of the name of a parameter", () => {
    function f(a, b = a) {
        function a() {}
        return [typeof a, typeof b];
    }
    expect(f(1)).toEqual(["function", "number"]);

    function g(a, b = () => a) {
        function a() {}
        return [typeof a, typeof b()];
    }
    expect(g(1)).toEqual(["function", "number"]);
});

test("a var called arguments", () => {
    function f(a = () => arguments) {
        var arguments;
        return [typeof arguments, a() === arguments];
    }
    expect(f()).toEqual(["object", true]);

    function g(a = () => arguments) {
        var arguments = 5;
        return [arguments, typeof a()];
    }
    expect(g()).toEqual([5, "object"]);
});

test("generators and async functions", () => {
    function* f(x, read = () => x) {
        var x = 2;
        yield [read(), x];
    }
    expect(f(1).next().value).toEqual([1, 2]);
});

test("a binding of the body that closures capture, seen from a loop of the body", () => {
    function f(read = () => captured) {
        var captured = 1;
        var last;
        for (var i = 0; i < 2; i++) {
            let copy = i;
            last = () => [copy, captured];
        }
        return last();
    }
    expect(f()).toEqual([1, 1]);
});

test("a function in a block does not get hoisted to a parameter of the same name", () => {
    function f(g = 123) {
        const before = g;
        {
            function g() {}
        }
        return [before, g];
    }
    expect(f()).toEqual([123, 123]);
});
