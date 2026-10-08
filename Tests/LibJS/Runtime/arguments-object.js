test("basic arguments object", () => {
    function foo() {
        return arguments.length;
    }
    expect(foo()).toBe(0);
    expect(foo(1)).toBe(1);
    expect(foo(1, 2)).toBe(2);
    expect(foo(1, 2, 3)).toBe(3);

    function bar() {
        return arguments[1];
    }
    expect(bar("hello", "friends", ":^)")).toBe("friends");
    expect(bar("hello")).toBe(undefined);
});

test("mapped arguments of separate calls", () => {
    function f(a, b, c) {
        const result = [arguments.length, arguments[0], arguments[1], arguments[2]];
        a = "a";
        arguments[1] = "b";
        result.push(arguments[0], b);
        delete arguments[0];
        arguments[0] = "x";
        result.push(a, arguments[0]);
        return result;
    }
    expect(f(1, 2, 3)).toEqual([3, 1, 2, 3, "a", "b", "a", "x"]);
    expect(f(4, 5, 6)).toEqual([3, 4, 5, 6, "a", "b", "a", "x"]);
    // Parameters without an argument are not mapped.
    expect(f(7)).toEqual([1, 7, undefined, undefined, "a", undefined, "a", "x"]);
    expect(f()).toEqual([0, undefined, undefined, undefined, undefined, undefined, "a", "x"]);
    expect(f(1, 2, 3, 4)).toEqual([4, 1, 2, 3, "a", "b", "a", "x"]);

    function g(a) {
        delete arguments[0];
        return arguments;
    }
    function h(a) {
        a = 2;
        return arguments[0];
    }
    expect(g(1)[0]).toBeUndefined();
    expect(h(1)).toBe(2);
});

test("mapped arguments with repeated parameter names", () => {
    function f(a, a, b) {
        a = "changed";
        return [arguments[0], arguments[1], arguments[2]];
    }
    expect(f(1, 2, 3)).toEqual([1, "changed", 3]);
    expect(f(1)).toEqual([1, undefined, undefined]);

    function g(a, b, a) {
        arguments[0] = "first";
        arguments[2] = "last";
        return [a, b];
    }
    expect(g(1, 2, 3)).toEqual(["last", 2]);
    expect(g(1, 2)).toEqual([undefined, 2]);
});

test("unmapping an argument does not affect other calls", () => {
    function f(a, b) {
        return arguments;
    }
    const first = f(1, 2);
    const second = f(3, 4);
    Object.defineProperty(first, "0", { value: 10, writable: false });
    Object.defineProperty(first, "1", { get: () => 20 });
    expect(first[0]).toBe(10);
    expect(first[1]).toBe(20);
    expect(second[0]).toBe(3);
    expect(second[1]).toBe(4);
    expect(f(5, 6)[0]).toBe(5);
});

test("many mapped parameters preserve the last duplicate", () => {
    const names = Array.from({ length: 4096 }, (_, index) => `parameter${index}`);
    names.push("parameter0");
    const f = Function(
        ...names,
        `
        parameter0 = "changed";
        parameter4095 = "last unique";
        return [arguments[0], arguments[4095], arguments[4096]];
    `
    );
    expect(f(...names)).toEqual(["parameter0", "last unique", "changed"]);
    expect(f("first")).toEqual(["first", undefined, undefined]);
});

test("arguments objects that gain named properties", () => {
    function mapped(a, b) {
        return arguments;
    }
    function unmapped(a, b) {
        "use strict";
        return arguments;
    }
    for (const make of [mapped, unmapped]) {
        const object = make(1, 2);
        for (let i = 0; i < 10; ++i) object[`key${i}`] = { i };
        gc();
        expect(object.length).toBe(2);
        expect(object[Symbol.iterator]).toBe(Array.prototype.values);
        expect(object.key9.i).toBe(9);
        expect([...object]).toEqual([1, 2]);
        delete object.length;
        expect(object.length).toBeUndefined();
        expect(Object.prototype.toString.call(object)).toBe("[object Arguments]");
    }
});

test("arguments of the enclosing function in arrow functions", () => {
    function viaArrow(a) {
        const read = () => arguments[0];
        a = 2;
        return read();
    }
    expect(viaArrow(1)).toBe(2);

    function viaNestedArrows(a) {
        return () => () => {
            a = 3;
            return arguments[0];
        };
    }
    expect(viaNestedArrows(1)()()).toBe(3);

    function writeThroughArrow(a, b) {
        (() => {
            arguments[1] = "written";
        })();
        return b;
    }
    expect(writeThroughArrow(1, 2)).toBe("written");
});

test("functions whose arguments object is never used", () => {
    const add = new Function("a", "b", "return a + b;");
    expect(add(1, 2)).toBe(3);
    function withParametersAndArguments(a, b) {
        return [a, b, arguments.length];
    }
    expect(withParametersAndArguments(1)).toEqual([1, undefined, 1]);
});
