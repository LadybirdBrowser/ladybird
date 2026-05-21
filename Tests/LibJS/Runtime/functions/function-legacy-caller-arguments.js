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

test("legacy caller and arguments properties appear in own property keys", () => {
    function ordinary() {}

    let ownPropertyNames = Object.getOwnPropertyNames(ordinary);
    expect(ownPropertyNames).toContain("arguments");
    expect(ownPropertyNames).toContain("caller");

    let ownKeys = Reflect.ownKeys(ordinary);
    expect(ownKeys).toContain("arguments");
    expect(ownKeys).toContain("caller");
});

test("legacy caller and arguments properties materialized by defineProperty are stable", () => {
    function getCallerDescriptor() {
        return Reflect.getOwnPropertyDescriptor(getCallerDescriptor, "caller");
    }

    function callGetCallerDescriptor() {
        return getCallerDescriptor();
    }

    expect(
        Reflect.defineProperty(getCallerDescriptor, "caller", {
            writable: false,
            configurable: false,
        })
    ).toBeTrue();

    let callerDescriptor = Reflect.getOwnPropertyDescriptor(getCallerDescriptor, "caller");
    expect(callerDescriptor.value).toBeNull();
    expect(callGetCallerDescriptor().value).toBe(callerDescriptor.value);

    function getArgumentsDescriptor() {
        return Reflect.getOwnPropertyDescriptor(getArgumentsDescriptor, "arguments");
    }

    expect(
        Reflect.defineProperty(getArgumentsDescriptor, "arguments", {
            writable: false,
            configurable: false,
        })
    ).toBeTrue();

    let argumentsDescriptor = Reflect.getOwnPropertyDescriptor(getArgumentsDescriptor, "arguments");
    expect(argumentsDescriptor.value).toBeNull();
    expect(getArgumentsDescriptor(1, 2, 3).value).toBe(argumentsDescriptor.value);
});
