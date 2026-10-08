// These run as strict mode scripts, whose global-looking assignment targets are resolved before the right-hand side
// is evaluated.
function runStrictScript(source) {
    return new Function(`"use strict"; ${source}`)();
}

test("assignments to declared globals", () => {
    globalThis.strictGlobalCounter = 0;
    runStrictScript("for (let i = 0; i < 10; ++i) strictGlobalCounter = strictGlobalCounter + 1;");
    expect(globalThis.strictGlobalCounter).toBe(10);
    delete globalThis.strictGlobalCounter;
});

test("assignments to unresolvable references throw after the right-hand side", () => {
    delete globalThis.strictGlobalLater;
    expect(() => runStrictScript("strictGlobalLater = (globalThis.strictGlobalLater = 1);")).toThrowWithMessage(
        ReferenceError,
        "'strictGlobalLater' is not defined"
    );
    expect(globalThis.strictGlobalLater).toBe(1);
    runStrictScript("strictGlobalLater = 2;");
    expect(globalThis.strictGlobalLater).toBe(2);
    delete globalThis.strictGlobalLater;
    expect(() => runStrictScript("strictGlobalLater = 3;")).toThrow(ReferenceError);
});

test("bindings deleted by the right-hand side", () => {
    globalThis.strictGlobalDeleted = 1;
    expect(() =>
        runStrictScript("strictGlobalDeleted = (delete globalThis.strictGlobalDeleted, 2);")
    ).toThrowWithMessage(ReferenceError, "'strictGlobalDeleted' is not defined");
    expect(globalThis.strictGlobalDeleted).toBeUndefined();
});

test("read-only and accessor globals", () => {
    Object.defineProperty(globalThis, "strictGlobalReadOnly", { value: 1, writable: false, configurable: true });
    expect(() => runStrictScript("strictGlobalReadOnly = 2;")).toThrow(TypeError);
    delete globalThis.strictGlobalReadOnly;

    let stored;
    Object.defineProperty(globalThis, "strictGlobalAccessor", {
        set(value) {
            stored = value;
        },
        configurable: true,
    });
    runStrictScript("strictGlobalAccessor = 3; strictGlobalAccessor = 4;");
    expect(stored).toBe(4);
    delete globalThis.strictGlobalAccessor;
});
