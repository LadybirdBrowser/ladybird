describe("function declarations in if statement clauses", () => {
    test("if clause", () => {
        if (true) function foo() {}
        if (false) function bar() {}
        expect(typeof foo).toBe("function");
        expect(typeof bar).toBe("undefined");
    });

    test("else clause", () => {
        if (false);
        else function foo() {}
        if (true);
        else function bar() {}
        expect(typeof foo).toBe("function");
        expect(typeof bar).toBe("undefined");
    });

    test("if and else clause", () => {
        if (true) function foo() {}
        else function bar() {}
        expect(typeof foo).toBe("function");
        expect(typeof bar).toBe("undefined");
    });

    test("syntax error in strict mode", () => {
        expect(`
            "use strict";
            if (true) function foo() {}
        `).not.toEval();
        expect(`
            "use strict";
            if (false);
            else function foo() {}
        `).not.toEval();
        expect(`
            "use strict";
            if (false) function foo() {}
            else function bar() {}
        `).not.toEval();
    });
});
