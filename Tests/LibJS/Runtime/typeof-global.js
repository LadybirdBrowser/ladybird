const iterations = 5;

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
