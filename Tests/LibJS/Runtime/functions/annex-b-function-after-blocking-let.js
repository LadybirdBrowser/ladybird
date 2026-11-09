test("a let later in an enclosing block keeps a function from hoisting", () => {
    function run() {
        var outer;
        {
            function x() {
                return 1;
            }
            outer = x;
        }
        {
            {
                function x() {
                    return 2;
                }
            }
            let x;
        }
        {
            let x;
            {
                function x() {
                    return 3;
                }
            }
        }
        return x === outer;
    }
    expect(run()).toBeTrue();
});

test("a let later in the function keeps a function from hoisting", () => {
    function run() {
        {
            function x() {
                return 1;
            }
        }
        let x = 2;
        return x;
    }
    expect(run()).toBe(2);
});

test("a var or function declaration later in the function does not keep a function from hoisting", () => {
    function varAfter() {
        {
            function x() {}
        }
        var x;
        return typeof x;
    }
    expect(varAfter()).toBe("function");

    function functionAfter() {
        {
            function x() {
                return "block";
            }
        }
        function x() {
            return "function";
        }
        return x();
    }
    expect(functionAfter()).toBe("block");

    function ifClause() {
        if (true) function x() {}
        var x;
        return typeof x;
    }
    expect(ifClause()).toBe("function");

    function laterBindingsKeepTheirPlaces() {
        {
            function x() {}
        }
        var x;
        var y = 1;
        const read = () => y;
        y = 2;
        return [typeof x, read()];
    }
    expect(laterBindingsKeepTheirPlaces()).toEqual(["function", 2]);
});

test("a function expression's own name does not keep a function of that name from hoisting", () => {
    var depth = 0;
    var f = function x() {
        var before = typeof x;
        if (++depth > 1) return "called the function expression again";
        {
            function x() {
                return "block";
            }
        }
        return [before, x()];
    };
    expect(f()).toEqual(["undefined", "block"]);
});

test("a function named arguments hoists in script and eval code that reads arguments", () => {
    try {
        expect((0, eval)("{ function arguments() {} } typeof arguments")).toBe("function");
    } finally {
        delete globalThis.arguments;
    }
    expect(eval("{ function arguments() {} } typeof arguments")).toBe("function");
});
