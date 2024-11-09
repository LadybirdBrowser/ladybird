test("constructors are always strict mode", () => {
    class A {
        constructor() {
            expect(isStrictMode()).toBeTrue();
        }
    }

    new A();
});

test("methods are always strict mode", () => {
    class A {
        method() {
            expect(isStrictMode()).toBeTrue();
        }
    }

    new A().method();
});
