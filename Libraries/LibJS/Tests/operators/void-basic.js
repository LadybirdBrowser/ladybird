test("basic functionality", () => {
    expect(void "").toBeUndefined();
    expect(void "foo").toBeUndefined();
    expect(void 1).toBeUndefined();
    expect(void 42).toBeUndefined();
    expect(void true).toBeUndefined();
    expect(void false).toBeUndefined();
    expect(void null).toBeUndefined();
    expect(void undefined).toBeUndefined();
    expect(void function () {}).toBeUndefined();
    expect(void (() => {})).toBeUndefined();
    expect(void (() => "hello friends")()).toBeUndefined();
    expect((() => void "hello friends")()).toBeUndefined();
});

describe("errors", () => {
    test("treats yield in generator as non variable", () => {
        expect("function f() { void yield; }").toEval();
        expect("async function f() { void yield; }").toEval();
        expect("function *f() { void yield; }").not.toEval();
        expect("async function *f() { void yield; }").not.toEval();

        expect("class C { *f() { void yield; } }").not.toEval();
        expect("var obj = { async function *f() { void yield; } }").not.toEval();
    });
});
