// JIT code computes `typeof` inline. These tests run every function many
// times, so that with a low JIT threshold they are compiled.

const iterations = 100;

function typeOf(value) {
    return typeof value;
}

function isFunction(value) {
    return typeof value === "function";
}

// Every comparison of typeof with a constant, strict and loose, and their
// negations.
function kinds(value) {
    const result = [];
    if (typeof value === "undefined") result.push("undefined");
    if (typeof value == "object") result.push("object");
    if (typeof value === "boolean") result.push("boolean");
    if (typeof value === "number") result.push("number");
    if (typeof value === "string") result.push("string");
    if (typeof value === "symbol") result.push("symbol");
    if (typeof value === "bigint") result.push("bigint");
    if (typeof value === "function") result.push("function");
    if (typeof value !== "object" && typeof value != "function") result.push("primitive");
    if ("number" === typeof value) result.push("reversed");
    if (typeof value === "Function") result.push("never");
    return result.join(",");
}

describe("typeof in JIT code", () => {
    test("every kind of value", () => {
        const values = [
            [undefined, "undefined"],
            [null, "object"],
            [true, "boolean"],
            [false, "boolean"],
            [0, "number"],
            [-1, "number"],
            [1.5, "number"],
            [-0, "number"],
            [NaN, "number"],
            [Infinity, "number"],
            ["", "string"],
            ["x", "string"],
            ["a longer string", "string"],
            [Symbol("s"), "symbol"],
            [1n, "bigint"],
            [{}, "object"],
            [[], "object"],
            [/x/, "object"],
            [new Proxy({}, {}), "object"],
            [function () {}, "function"],
            [() => {}, "function"],
            [class {}, "function"],
            [Math.max, "function"],
            [function () {}.bind(null), "function"],
            [new Proxy(function () {}, {}), "function"],
            [async function () {}, "function"],
            [function* () {}, "function"],
        ];
        for (let i = 0; i < iterations; ++i) {
            for (const [value, expected] of values) {
                expect(typeOf(value)).toBe(expected);
                expect(isFunction(value)).toBe(expected === "function");
                const primitive = expected !== "object" && expected !== "function" ? ",primitive" : "";
                const reversed = expected === "number" ? ",reversed" : "";
                expect(kinds(value)).toBe(expected + primitive + reversed);
            }
            expect(typeOf("rope " + i)).toBe("string");
        }
    });
});

var globalNumber = 1;
let globalLet = "s";
const globalConst = {};
function globalFunction() {}

describe("typeof of global variables", () => {
    test("variables of the global object and the global declarative environment", () => {
        function types() {
            return [
                typeof globalNumber,
                typeof globalLet,
                typeof globalConst,
                typeof globalFunction,
                typeof notDeclaredAnywhere,
                typeof Math,
            ];
        }
        for (let i = 0; i < iterations; ++i)
            expect(types()).toEqual(["number", "string", "object", "function", "undefined", "object"]);
        globalNumber = "now a string";
        globalLet = 1n;
        expect(types()).toEqual(["string", "bigint", "object", "function", "undefined", "object"]);
        globalThis.notDeclaredAnywhere = 1;
        expect(types()[4]).toBe("number");
        delete globalThis.notDeclaredAnywhere;
        expect(types()[4]).toBe("undefined");
    });

    test("global properties that change", () => {
        function type() {
            return typeof changingGlobal;
        }
        globalThis.changingGlobal = 1;
        for (let i = 0; i < iterations; ++i) expect(type()).toBe("number");
        delete globalThis.changingGlobal;
        expect(type()).toBe("undefined");
        let getterCalls = 0;
        Object.defineProperty(globalThis, "changingGlobal", {
            get() {
                ++getterCalls;
                return "from a getter";
            },
            configurable: true,
        });
        expect(type()).toBe("string");
        expect(getterCalls).toBe(1);
        delete globalThis.changingGlobal;
    });

    test("global lexical bindings in their temporal dead zone throw", () => {
        expect(() => typeof laterGlobalLet).toThrow(ReferenceError);
    });
});

let laterGlobalLet = 1;
