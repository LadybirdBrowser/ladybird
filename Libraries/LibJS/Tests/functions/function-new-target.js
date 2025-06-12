test("basic functionality", () => {
    function foo() {
        return new.target;
    }
    expect(foo()).toBeUndefined();
    expect(new foo()).toEqual(foo);

    function bar() {
        const baz = () => new.target;
        return baz();
    }
    expect(bar()).toBeUndefined();
    expect(new bar()).toEqual(bar);

    class baz {
        constructor() {
            this.newTarget = new.target;
        }
    }
    expect(new baz().newTarget).toEqual(baz);
});

test("retrieving new.target from direct eval", () => {
    function foo() {
        return eval("new.target");
    }

    let result;

    expect(() => {
        result = foo();
    }).not.toThrowWithMessage(SyntaxError, "'new.target' not allowed outside of a function");

    expect(result).toBe(undefined);

    expect(() => {
        result = new foo();
    }).not.toThrowWithMessage(SyntaxError, "'new.target' not allowed outside of a function");

    expect(result).toBe(foo);
});

test("cannot retrieve new.target from indirect eval", () => {
    const indirect = eval;

    function foo() {
        return indirect("new.target");
    }

    expect(() => {
        foo();
    }).toThrowWithMessage(SyntaxError, "'new.target' not allowed outside of a function");

    expect(() => {
        new foo();
    }).toThrowWithMessage(SyntaxError, "'new.target' not allowed outside of a function");
});

test("syntax error outside of function", () => {
    expect("new.target").not.toEval();
});
