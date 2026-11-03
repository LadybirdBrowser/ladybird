test("legacy caller property exposes only ordinary sloppy callers", () => {
    function getCaller() {
        return getCaller.caller;
    }

    function ordinary() {
        return getCaller();
    }

    function strict() {
        "use strict";
        return getCaller();
    }

    function* generator() {
        yield getCaller();
    }

    expect(getCaller()).toBeNull();
    expect(ordinary()).toBe(ordinary);
    expect(strict()).toBeNull();
    expect(generator().next().value).toBeNull();
    expect([1].map(getCaller)[0]).toBeNull();
});

test("legacy arguments property exposes the active arguments object", () => {
    function getArguments(a, b) {
        return getArguments.arguments;
    }

    expect(getArguments.arguments).toBeNull();

    let argumentsObject = getArguments(1, undefined);
    expect(argumentsObject.length).toBe(2);
    expect(argumentsObject[0]).toBe(1);
    expect(argumentsObject[1]).toBeUndefined();
    expect(argumentsObject.callee).toBe(getArguments);

    let calleeDescriptor = Reflect.getOwnPropertyDescriptor(argumentsObject, "callee");
    expect(calleeDescriptor.value).toBe(getArguments);
    expect(calleeDescriptor.writable).toBeTrue();
    expect(calleeDescriptor.enumerable).toBeFalse();
    expect(calleeDescriptor.configurable).toBeTrue();

    function getClonedArguments(a) {
        let legacyArguments = getClonedArguments.arguments;
        a = 9;
        return legacyArguments;
    }

    let clonedArguments = getClonedArguments(7);
    expect(clonedArguments[0]).toBe(7);
    expect(clonedArguments.callee).toBe(getClonedArguments);

    function getRestrictedArguments(a = 1) {
        return getRestrictedArguments.arguments;
    }

    expect(() => getRestrictedArguments().callee).toThrow(TypeError);

    expect(getArguments.arguments).toBeNull();
});

test("restricted functions keep the throwing caller and arguments accessors", () => {
    function strict() {
        "use strict";
    }

    let arrow = () => {};

    expect(() => strict.caller).toThrow(TypeError);
    expect(() => strict.arguments).toThrow(TypeError);
    expect(() => arrow.caller).toThrow(TypeError);
    expect(() => arrow.arguments).toThrow(TypeError);
});

test("legacy caller and arguments are not own properties of functions", () => {
    function ordinary() {
        expect(Object.getOwnPropertyDescriptor(ordinary, "caller")).toBeUndefined();
        expect(Object.getOwnPropertyDescriptor(ordinary, "arguments")).toBeUndefined();
    }
    ordinary();

    expect(Object.getOwnPropertyNames(ordinary)).not.toContain("caller");
    expect(Object.getOwnPropertyNames(ordinary)).not.toContain("arguments");
    expect(Reflect.ownKeys(ordinary)).not.toContain("caller");
    expect(Reflect.ownKeys(ordinary)).not.toContain("arguments");
    expect(Object.hasOwn(ordinary, "caller")).toBeFalse();
    expect(Object.hasOwn(ordinary, "arguments")).toBeFalse();
    expect("caller" in ordinary).toBeTrue();
    expect("arguments" in ordinary).toBeTrue();
});

test("walking own properties from inside a function does not reach its caller's arguments", () => {
    const secret = {};

    function walk(root) {
        const seen = [];
        (function visit(object) {
            if (!object || (typeof object !== "object" && typeof object !== "function") || seen.includes(object))
                return;
            seen.push(object);
            for (const name of Object.getOwnPropertyNames(object)) {
                const descriptor = Object.getOwnPropertyDescriptor(object, name);
                if ("value" in descriptor) visit(descriptor.value);
            }
        })(root);
        return seen;
    }

    function outer(argument) {
        const holder = {
            method: function () {
                return walk(holder);
            },
        };
        return holder.method();
    }

    expect(outer(secret)).not.toContain(secret);
});

test("%Function.prototype% has caller and arguments accessors", () => {
    function strict() {
        "use strict";
    }

    for (const name of ["caller", "arguments"]) {
        const descriptor = Object.getOwnPropertyDescriptor(Function.prototype, name);
        expect(descriptor.get).toBeInstanceOf(Function);
        expect(descriptor.set).toBeInstanceOf(Function);
        expect(descriptor.enumerable).toBeFalse();
        expect(descriptor.configurable).toBeTrue();

        expect(descriptor.get.call(function () {})).toBeNull();
        expect(descriptor.set.call(function () {}, 1)).toBeUndefined();

        const restricted = [strict, () => {}, class {}, function* () {}, async function () {}, Math.abs, {}, 1];
        restricted.push(function () {}.bind(), { method() {} }.method, Function.prototype, undefined);
        for (const receiver of restricted) {
            expect(() => descriptor.get.call(receiver)).toThrow(TypeError);
            expect(() => descriptor.set.call(receiver, 1)).toThrow(TypeError);
        }
    }
});

test("objects that inherit from sloppy functions cannot use legacy caller and arguments", () => {
    function base() {}
    class Derived extends base {}

    for (const object of [Object.create(base), new Proxy(base, {}), Derived]) {
        expect(() => object.caller).toThrowWithMessage(TypeError, "ordinary non-strict functions");
        expect(() => object.arguments).toThrowWithMessage(TypeError, "ordinary non-strict functions");
    }

    const inheriting = Object.create(base);
    expect(() => {
        inheriting.caller = 1;
    }).toThrow(TypeError);
    expect(() => {
        inheriting.arguments = 1;
    }).toThrow(TypeError);
});

test("assigning to legacy caller and arguments does nothing", () => {
    function ordinary() {
        ordinary.caller = 1;
        ordinary.arguments = 2;
        return [ordinary.caller, ordinary.arguments];
    }

    const [caller, argumentsObject] = ordinary(3);
    expect(caller).toBeNull();
    expect(argumentsObject[0]).toBe(3);
    expect(Object.hasOwn(ordinary, "caller")).toBeFalse();
    expect(Object.hasOwn(ordinary, "arguments")).toBeFalse();
});

test("own caller and arguments properties shadow the legacy accessors", () => {
    function ordinary() {
        return [ordinary.caller, ordinary.arguments];
    }

    expect(Reflect.defineProperty(ordinary, "caller", { value: "own caller" })).toBeTrue();
    expect(Reflect.defineProperty(ordinary, "arguments", { value: "own arguments" })).toBeTrue();
    expect(ordinary()).toEqual(["own caller", "own arguments"]);
});

test("freezing a function keeps its legacy caller and arguments working", () => {
    function ordinary() {
        return ordinary.arguments;
    }

    Object.freeze(ordinary);
    expect(ordinary(1)[0]).toBe(1);
    expect(ordinary.arguments).toBeNull();
});
