test("named function expressions keep names used by parameter expressions", () => {
    const direct = function named(value = named) {
        return value;
    };
    expect(direct()).toBe(direct);

    const nested = function named(value = () => named) {
        return value;
    };
    expect(nested()()).toBe(nested);

    const evaluated = function named(value = eval("named")) {
        return value;
    };
    expect(evaluated()).toBe(evaluated);
});

test("nested eval keeps the enclosing function name visible", () => {
    const functionWithEval = function named() {
        return () => eval("named");
    };
    expect(functionWithEval()()).toBe(functionWithEval);
});

test("unused name scopes preserve function names and enclosing bindings", () => {
    let value = 1;
    const functionWithUnusedName = function named() {
        return () => value;
    };
    value = 2;
    expect(functionWithUnusedName.name).toBe("named");
    expect(functionWithUnusedName()()).toBe(2);
    expect(typeof named).toBe("undefined");
});
