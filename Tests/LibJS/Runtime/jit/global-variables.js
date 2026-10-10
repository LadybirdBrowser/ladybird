// JIT code reads and writes global variables directly: global `let`, `const`
// and `class` bindings, and properties of the global object. It uses the
// values of `const` bindings as constants, and speculates that globals
// holding objects keep them. These tests run every function many times, so
// that with a low JIT threshold they are compiled, and then change what the
// compiled code relied on.

const iterations = 200;
const constantNumber = 3;
const constantObject = { value: 4 };
let changingLet = 1;
let changingObject = { value: 1 };
var changingVar = 1;
var functionVar = () => "first";
class GlobalClass {
    static value = 5;
}

function readConstants() {
    return constantNumber + constantObject.value + GlobalClass.value;
}

function readLet() {
    return changingLet;
}

function readObjectLet() {
    return changingObject.value;
}

function writeLet(value) {
    changingLet = value;
}

function readVar() {
    return changingVar;
}

function writeVar(value) {
    changingVar = value;
}

function callFunctionVar() {
    return functionVar();
}

describe("Global variables in JIT code", () => {
    test("constants", () => {
        for (let i = 0; i < iterations; ++i) expect(readConstants()).toBe(12);
    });

    test("let bindings that change", () => {
        for (let i = 0; i < iterations; ++i) {
            writeLet(i);
            expect(readLet()).toBe(i);
        }
        writeLet("a string");
        expect(readLet()).toBe("a string");
        for (let i = 0; i < iterations; ++i) expect(readObjectLet()).toBe(1);
        changingObject = { value: 2 };
        expect(readObjectLet()).toBe(2);
    });

    test("var properties that change", () => {
        for (let i = 0; i < iterations; ++i) {
            writeVar(i);
            expect(readVar()).toBe(i);
        }
        writeVar(undefined);
        expect(readVar()).toBeUndefined();
        for (let i = 0; i < iterations; ++i) expect(callFunctionVar()).toBe("first");
        functionVar = () => "second";
        expect(callFunctionVar()).toBe("second");
        globalThis.functionVar = () => "third";
        expect(callFunctionVar()).toBe("third");
    });

    test("properties of the global object that become accessors or go away", () => {
        globalThis.configurableGlobal = 1;
        const read = () => configurableGlobal;
        const write = value => {
            configurableGlobal = value;
        };
        for (let i = 0; i < iterations; ++i) {
            write(i);
            expect(read()).toBe(i);
        }
        let stored;
        Object.defineProperty(globalThis, "configurableGlobal", {
            get: () => "getter",
            set: value => {
                stored = value;
            },
            configurable: true,
        });
        expect(read()).toBe("getter");
        write(7);
        expect(stored).toBe(7);
        delete globalThis.configurableGlobal;
        expect(() => read()).toThrowWithMessage(ReferenceError, "'configurableGlobal' is not defined");
    });

    test("bindings that a later script declares shadow properties of the global object", () => {
        globalThis.shadowedGlobal = "property";
        const read = () => shadowedGlobal;
        const values = [];
        for (let i = 0; i < iterations; ++i) {
            if (i === iterations / 2) evaluateSource("let shadowedGlobal = 'binding';");
            values.push(read());
        }
        expect(values[0]).toBe("property");
        expect(values[iterations / 2 - 1]).toBe("property");
        expect(values[iterations / 2]).toBe("binding");
        expect(values[iterations - 1]).toBe("binding");
        expect(globalThis.shadowedGlobal).toBe("property");
    });

    test("bindings that are not initialized yet", () => {
        evaluateSource(`
            function readEarly() {
                return earlyBinding;
            }
            globalThis.earlyResults = [];
            for (let i = 0; i < ${iterations}; ++i) {
                try {
                    earlyResults.push(readEarly());
                } catch (error) {
                    earlyResults.push(error.constructor.name);
                }
            }
            let earlyBinding = "initialized";
            for (let i = 0; i < ${iterations}; ++i) earlyResults.push(readEarly());
        `);
        expect(earlyResults.length).toBe(2 * iterations);
        expect(earlyResults.slice(0, iterations).every(result => result === "ReferenceError")).toBeTrue();
        expect(earlyResults.slice(iterations).every(result => result === "initialized")).toBeTrue();
    });

    test("constant bindings cannot be assigned", () => {
        const assign = () => {
            constantNumber = 4;
        };
        for (let i = 0; i < iterations; ++i)
            expect(assign).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
        expect(readConstants()).toBe(12);
    });
});
