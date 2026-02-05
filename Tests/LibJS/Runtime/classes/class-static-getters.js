describe("correct behavior", () => {
    test("basic functionality", () => {
        class A {
            static get x() {
                return 10;
            }
        }

        expect(A.x).toBe(10);
        expect(new A()).not.toHaveProperty("x");
    });

    test("name", () => {
        class A {
            static get x() {}
        }

        const d = Object.getOwnPropertyDescriptor(A, "x");
        expect(d.get.name).toBe("get x");
    });

    test("extended name syntax", () => {
        const s = Symbol("foo");

        class A {
            static get "method with space"() {
                return 1;
            }

            static get 12() {
                return 2;
            }

            static get [`he${"llo"}`]() {
                return 3;
            }

            static get [s]() {
                return 4;
            }
        }

        expect(A["method with space"]).toBe(1);
        expect(A[12]).toBe(2);
        expect(A.hello).toBe(3);
        expect(A[s]).toBe(4);
    });

    test("inherited static getter", () => {
        class Parent {
            static get x() {
                return 3;
            }
        }

        class Child extends Parent {}

        expect(Parent.x).toBe(3);
        expect(Child.x).toBe(3);
    });

    test("inherited static getter overriding", () => {
        class Parent {
            static get x() {
                return 3;
            }
        }

        class Child extends Parent {
            static get x() {
                return 10;
            }
        }

        expect(Parent.x).toBe(3);
        expect(Child.x).toBe(10);
    });
});

describe("errors", () => {
    test('"get static" is a syntax error', () => {
        expect(`
    class A {
      get static foo() {}
    }`).not.toEval();
    });
});

test("static getter named 'async'", () => {
    class A {
        static get async() {
            return "static getter named async";
        }
    }

    expect("async" in A).toBeTrue();
    expect(A.async).toBe("static getter named async");
});
