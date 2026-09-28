test("basic that non-strict direct eval() prevents non-local access caching", () => {
    function foo(do_eval) {
        var c = 1;
        function bar(do_eval) {
            if (do_eval) eval("var c = 2;");
            return c;
        }
        return bar(do_eval);
    }

    expect(foo(false)).toBe(1);
    expect(foo(true)).toBe(2);
});

test("eval var shadows a parameter in a function with parameter expressions", () => {
    function foo(a, b = () => a) {
        var c;
        eval("var a = 'eval'");
        a += "!";
        return [a, b()];
    }
    expect(foo("parameter")).toEqual(["eval!", "parameter"]);
});

test("eval function declaration shadows a parameter in a function with parameter expressions", () => {
    function foo(a, b = 1) {
        var c;
        {
            eval("function a() {}");
        }
        return typeof a;
    }
    expect(foo(1)).toBe("function");
});
