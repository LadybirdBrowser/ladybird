// JIT code runs `instanceof` with a global function on its right-hand side
// inline, walking the prototype chain of its left-hand side. These tests run
// every function many times, so that with a low JIT threshold they are
// compiled, and then change what the compiled code relied on.

const iterations = 100;

function GlobalFunction() {}
class GlobalClass {}
class DerivedClass extends GlobalClass {}
var boundFunction = GlobalFunction.bind(null);

function isGlobalFunction(value) {
    return value instanceof GlobalFunction;
}

function isGlobalClass(value) {
    return value instanceof GlobalClass;
}

function isArray(value) {
    return value instanceof Array;
}

function isBoundFunction(value) {
    return value instanceof boundFunction;
}

describe("instanceof in JIT code", () => {
    test("instances, their prototypes and other values", () => {
        const instance = new GlobalFunction();
        const derived = new DerivedClass();
        const values = [
            [instance, true, false],
            [Object.create(instance), true, false],
            [new GlobalClass(), false, true],
            [derived, false, true],
            [GlobalFunction.prototype, false, false],
            [{}, false, false],
            [Object.create(null), false, false],
            [[], false, false],
            [() => {}, false, false],
            [1, false, false],
            ["string", false, false],
            [null, false, false],
            [undefined, false, false],
        ];
        for (let i = 0; i < iterations; ++i) {
            for (const [value, isFunctionInstance, isClassInstance] of values) {
                expect(isGlobalFunction(value)).toBe(isFunctionInstance);
                expect(isGlobalClass(value)).toBe(isClassInstance);
                expect(isBoundFunction(value)).toBe(isFunctionInstance);
            }
            expect(isArray([])).toBeTrue();
            expect(isArray({})).toBeFalse();
        }
    });

    test("prototype chains that change", () => {
        const object = {};
        for (let i = 0; i < iterations; ++i) expect(isGlobalFunction(object)).toBeFalse();
        Object.setPrototypeOf(object, GlobalFunction.prototype);
        expect(isGlobalFunction(object)).toBeTrue();
        const proxy = new Proxy(new GlobalFunction(), {});
        expect(isGlobalFunction(proxy)).toBeTrue();
        expect(isGlobalFunction(Object.create(proxy))).toBeTrue();
        const trapped = new Proxy(
            {},
            {
                getPrototypeOf: () => GlobalFunction.prototype,
            }
        );
        expect(isGlobalFunction(trapped)).toBeTrue();
    });

    test("prototype properties that change", () => {
        const instance = new GlobalFunction();
        for (let i = 0; i < iterations; ++i) expect(isGlobalFunction(instance)).toBeTrue();
        const originalPrototype = GlobalFunction.prototype;
        GlobalFunction.prototype = {};
        expect(isGlobalFunction(instance)).toBeFalse();
        expect(isGlobalFunction(new GlobalFunction())).toBeTrue();
        GlobalFunction.prototype = 1;
        expect(() => isGlobalFunction(instance)).toThrowWithMessage(TypeError, "'prototype' property of");
        expect(isGlobalFunction(1)).toBeFalse();
        GlobalFunction.prototype = originalPrototype;
        expect(isGlobalFunction(instance)).toBeTrue();
    });

    test("functions that get their own @@hasInstance", () => {
        const instance = new GlobalClass();
        for (let i = 0; i < iterations; ++i) expect(isGlobalClass(instance)).toBeTrue();
        Object.defineProperty(GlobalClass, Symbol.hasInstance, {
            value: value => value === 42,
            configurable: true,
        });
        expect(isGlobalClass(instance)).toBeFalse();
        expect(isGlobalClass(42)).toBeTrue();
        delete GlobalClass[Symbol.hasInstance];
        expect(isGlobalClass(instance)).toBeTrue();
        // %Function.prototype%[@@hasInstance] can never change.
        Function.prototype[Symbol.hasInstance] = () => true;
        expect(isGlobalClass({})).toBeFalse();
    });
});
