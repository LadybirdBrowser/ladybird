test("does not crash when accessing super.length", () => {
    let result;

    class A {
        constructor() {}

        get length() {
            return 2;
        }
    }

    class B extends A {
        constructor() {
            super();
            result = super.length;
        }
    }

    new B();

    expect(result).toBe(2);
});
