describe("named function expression scoping", () => {
    test("name is visible inside function body", () => {
        const f = function factorial(n) {
            if (n <= 1) return 1;
            return n * factorial(n - 1);
        };
        expect(f(5)).toBe(120);
    });

    test("name is not visible outside function body", () => {
        const f = function inner() {
            return 42;
        };
        expect(() => inner).toThrowWithMessage(ReferenceError, "'inner' is not defined");
    });

    test("name binding is immutable inside function", () => {
        const f = function immutableName() {
            immutableName = "changed";
            return typeof immutableName;
        };
        expect(f()).toBe("function");
    });

    test("outer variable with same name is shadowed inside", () => {
        let shadow = "outer";
        const f = function shadow() {
            return typeof shadow;
        };
        expect(shadow).toBe("outer");
        expect(f()).toBe("function");
    });

    test("assignment to outer variable with same name works", () => {
        let outer;
        outer = function outer() {
            return outer.name;
        };
        expect(outer()).toBe("outer");
        expect(typeof outer).toBe("function");
    });

    test("property access on outer variable with same name works", () => {
        let Foo;
        Foo = function Foo() {};
        Foo.bar = 42;
        expect(Foo.bar).toBe(42);
    });

    test("nested named function expressions", () => {
        const outer = function outerFn() {
            const inner = function innerFn() {
                return typeof outerFn + "," + typeof innerFn;
            };
            return inner();
        };
        expect(outer()).toBe("function,function");
    });

    test("same name in nested function expressions", () => {
        const outer = function sameName() {
            const inner = function sameName() {
                return sameName.length;
            };
            return [sameName.length, inner()];
        };
        const result = outer();
        expect(result[0]).toBe(0); // outer sameName
        expect(result[1]).toBe(0); // inner sameName (shadows outer)
    });
});
