describe("parsing freestanding generators", () => {
    test("simple", () => {
        expect(`async function* foo() {}`).toEval();
        expect(`async function *foo() {}`).toEval();
        expect(`async function
            *foo() {}`).toEval();

        expect(`async function *await() {}`).not.toEval();
        expect(`async function *yield() {}`).not.toEval();
    });
    test("yield & await expression", () => {
        expect(`async function* foo() { yield; await 1; }`).toEval();
        expect(`async function* foo() { yield (yield); await (yield); }`).toEval();
        expect(`async function* foo() { yield (yield foo); yield (await foo); }`).toEval();

        expect(`async function foo() { yield; }`).toEval();
        expect(`async function foo() { yield 3; }`).not.toEval();
        expect(`function* foo() { await 3; }`).not.toEval();
    });
    test("yield-from expression", () => {
        expect(`async function* foo() { yield *bar; }`).toEval();
        expect(`async function* foo() { yield *(yield); }`).toEval();
        expect(`async function* foo() { yield
            *bar; }`).not.toEval();
        expect(`async function foo() { yield
            *bar; }`).toEval();
    });
    test("for await only allows for-of syntax", () => {
        expect(`async function* foo() { for await (value of []) ; }`).toEval();

        expect(`async function* foo() { for await (;;) ; }`).not.toEval();
        expect(`async function* foo() { for await (value ;;) ; }`).not.toEval();
        expect(`async function* foo() { for await (value in []) ; }`).not.toEval();
    });
});

describe("parsing object literal generator functions", () => {
    test("simple", () => {
        expect(`x = { async *foo() { } }`).toEval();
        expect(`x = { async * foo() { } }`).toEval();
        expect(`x = { async *
                foo() { } }`).toEval();
    });
    test("yield & await", () => {
        expect(`x = { async foo() { yield; await 3;} }`).toEval();
        expect(`x = { async *foo() { yield; await 3; } }`).toEval();
        expect(`x = { async *foo() { yield 42; await 3; } }`).toEval();
        expect(`x = { async *foo() { yield (yield); await (yield); } }`).toEval();
        expect(`x = { async *
                foo() { yield (yield); await 4; } }`).toEval();

        expect(`x = { async foo() { yield 42; } }`).not.toEval();
        expect(`x = { *foo() { await 42; } }`).not.toEval();
    });
});

describe("parsing classes with generator methods", () => {
    test("simple", () => {
        expect(`class Foo { async *foo() {} }`).toEval();
        expect(`class Foo { static async *foo() {} }`).toEval();
        expect(`class Foo { async *foo() { yield; } }`).toEval();
        expect(`class Foo { async *foo() { yield 42; } }`).toEval();
        expect(`class Foo { async *constructor() { yield 42; } }`).not.toEval();
    });
});

test("async generator instances use the intrinsic prototype when function prototype is not an object", () => {
    async function* generator() {}
    const AsyncGeneratorPrototype = Object.getPrototypeOf(generator.prototype);

    for (const prototype of [undefined, null, false, "", Symbol(), 1]) {
        generator.prototype = prototype;
        expect(Object.getPrototypeOf(generator())).toBe(AsyncGeneratorPrototype);
    }
});
