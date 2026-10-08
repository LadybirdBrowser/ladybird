const readDefaultProps = component => component.defaultProps;
const readPrototype = value => value.prototype;
const readCaller = value => value.caller;
const readArguments = value => value.arguments;

test("missing properties of functions", () => {
    const components = [];
    for (let i = 0; i < 10; ++i) components.push(function () {});
    for (let round = 0; round < 3; ++round) {
        for (const component of components) expect(readDefaultProps(component)).toBeUndefined();
    }
    components[3].defaultProps = { a: 1 };
    expect(readDefaultProps(components[3])).toEqual({ a: 1 });
    expect(readDefaultProps(components[4])).toBeUndefined();
    Function.prototype.defaultProps = "inherited";
    try {
        expect(readDefaultProps(components[4])).toBe("inherited");
    } finally {
        delete Function.prototype.defaultProps;
    }
    expect(readDefaultProps(components[4])).toBeUndefined();
});

test("prototype of functions that create it lazily and of methods without one", () => {
    const methods = { method() {} };
    const arrow = () => {};
    for (let round = 0; round < 3; ++round) {
        expect(readPrototype(methods.method)).toBeUndefined();
        expect(readPrototype(arrow)).toBeUndefined();
        const lazy = function () {};
        const prototype = readPrototype(lazy);
        expect(typeof prototype).toBe("object");
        expect(prototype.constructor).toBe(lazy);
    }
});

test("legacy caller and arguments of sloppy functions", () => {
    const strict = function () {
        "use strict";
    };
    function sloppy() {
        return [readCaller(sloppy), readArguments(sloppy)];
    }
    for (let round = 0; round < 3; ++round) {
        expect(readCaller(sloppy)).toBeNull();
        expect(readArguments(sloppy)).toBeNull();
        const [, args] = sloppy(1, 2);
        expect(args.length).toBe(2);
        expect(() => readCaller(strict)).toThrow(TypeError);
    }
});
