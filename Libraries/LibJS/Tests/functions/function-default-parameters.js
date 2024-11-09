test("single default parameter", () => {
    function func(a = 6) {
        return typeof a;
    }

    const arrowFunc = (a = 6) => typeof a;

    expect(func()).toBe("number");
    expect(func(5)).toBe("number");
    expect(func(undefined)).toBe("number");
    expect(func(false)).toBe("boolean");
    expect(func(null)).toBe("object");
    expect(func({})).toBe("object");

    expect(arrowFunc()).toBe("number");
    expect(arrowFunc(5)).toBe("number");
    expect(arrowFunc(undefined)).toBe("number");
    expect(arrowFunc(false)).toBe("boolean");
    expect(arrowFunc(null)).toBe("object");
    expect(arrowFunc({})).toBe("object");
});

test("two parameters, second one is default", () => {
    function func(a, b = 1) {
        return a + b;
    }

    const arrowFunc = (a, b = 1) => a + b;

    expect(func(4, 5)).toBe(9);
    expect(func(4)).toBe(5);
    expect(func(4, undefined)).toBe(5);
    expect(func()).toBeNaN();

    expect(arrowFunc(4, 5)).toBe(9);
    expect(arrowFunc(4)).toBe(5);
    expect(arrowFunc(4, undefined)).toBe(5);
    expect(arrowFunc()).toBeNaN();
});

test("two parameters, first one is default", () => {
    function func(a = 5, b) {
        return a + b;
    }

    const arrowFunc = (a = 5, b) => a + b;

    expect(func(4, 5)).toBe(9);
    expect(func(undefined, 4)).toBe(9);
    expect(func()).toBeNaN();

    expect(arrowFunc(4, 5)).toBe(9);
    expect(arrowFunc(undefined, 4)).toBe(9);
    expect(arrowFunc()).toBeNaN();
});

test("default parameter references a previous parameter", () => {
    function func(a, b = a) {
        return a + b;
    }

    const arrowFunc = (a, b = a) => a + b;

    expect(func(4, 5)).toBe(9);
    expect(func(4)).toBe(8);
    expect(func("hf")).toBe("hfhf");
    expect(func(true)).toBe(2);
    expect(func()).toBeNaN();

    expect(arrowFunc(4, 5)).toBe(9);
    expect(arrowFunc(4)).toBe(8);
    expect(arrowFunc("hf")).toBe("hfhf");
    expect(arrowFunc(true)).toBe(2);
    expect(arrowFunc()).toBeNaN();
});

test("parameter with a function default value", () => {
    function func(
        a = function () {
            return 5;
        }
    ) {
        return a();
    }

    const arrowFunc = (
        a = function () {
            return 5;
        }
    ) => a();

    expect(func()).toBe(5);
    expect(
        func(function () {
            return 10;
        })
    ).toBe(10);
    expect(func(() => 10)).toBe(10);

    expect(arrowFunc()).toBe(5);
    expect(
        arrowFunc(function () {
            return 10;
        })
    ).toBe(10);
    expect(arrowFunc(() => 10)).toBe(10);
});

test("parameter with an arrow function default value", () => {
    function func(a = () => 5) {
        return a();
    }

    const arrowFunc = (a = () => 5) => a();

    expect(func()).toBe(5);
    expect(
        func(function () {
            return 10;
        })
    ).toBe(10);
    expect(func(() => 10)).toBe(10);
    expect(arrowFunc()).toBe(5);
    expect(
        arrowFunc(function () {
            return 10;
        })
    ).toBe(10);
    expect(arrowFunc(() => 10)).toBe(10);
});

test("parameter with an object default value", () => {
    function func(a = { foo: "bar" }) {
        return a.foo;
    }

    const arrowFunc = (a = { foo: "bar" }) => a.foo;

    expect(func()).toBe("bar");
    expect(func({ foo: "baz" })).toBe("baz");
    expect(arrowFunc()).toBe("bar");
    expect(arrowFunc({ foo: "baz" })).toBe("baz");
});

test("use variable as default function parameter", () => {
    let a = 1;

    function func(param = a) {
        return param;
    }

    expect(func()).toBe(a);
});

test("variable is initialized to the value of the parameter if one with the same name exists", () => {
    function func(a = 1) {
        var a;
        return a;
    }

    expect(func()).toBe(1);
});
