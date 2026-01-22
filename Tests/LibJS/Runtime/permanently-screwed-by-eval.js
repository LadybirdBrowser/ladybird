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
