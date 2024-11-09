describe("correct behavior", () => {
    test("basic functionality", () => {
        class A {
            static get x() {
                return this._x;
            }

            static set x(value) {
                this._x = value * 2;
            }
        }

        expect(A.x).toBeUndefined();
        expect(A).not.toHaveProperty("_x");
        A.x = 3;
        expect(A.x).toBe(6);
        expect(A).toHaveProperty("_x", 6);
    });

    test("name", () => {
        class A {
            static set x(v) {}
        }

        const d = Object.getOwnPropertyDescriptor(A, "x");
        expect(d.set.name).toBe("set x");
    });

    test("extended name syntax", () => {
        const s = Symbol("foo");

        class A {
            static set "method with space"(value) {
                this.a = value;
            }

            static set 12(value) {
                this.b = value;
            }

            static set [`he${"llo"}`](value) {
                this.c = value;
            }

            static set [s](value) {
                this.d = value;
            }
        }

        A["method with space"] = 1;
        A[12] = 2;
        A.hello = 3;
        A[s] = 4;
        expect(A.a).toBe(1);
        expect(A.b).toBe(2);
        expect(A.c).toBe(3);
        expect(A.d).toBe(4);
    });

    test("inherited static setter", () => {
        class Parent {
            static get x() {
                return this._x;
            }

            static set x(value) {
                this._x = value * 2;
            }
        }

        class Child extends Parent {}

        expect(Child.x).toBeUndefined();
        Child.x = 10;
        expect(Child.x).toBe(20);
    });

    test("inherited static setter overriding", () => {
        class Parent {
            static get x() {
                return this._x;
            }

            static set x(value) {
                this._x = value * 2;
            }
        }

        class Child extends Parent {
            static get x() {
                return this._x;
            }

            static set x(value) {
                this._x = value * 3;
            }
        }

        expect(Child.x).toBeUndefined();
        Child.x = 10;
        expect(Child.x).toBe(30);
    });
});

describe("errors", () => {
    test('"set static" is a syntax error', () => {
        expect(`
    class A {
      set static foo(value) {}
    }`).not.toEval();
    });
});

test("static setter named 'async'", () => {
    let called = false;
    class A {
        static set async(value) {
            called = true;
            expect(value).toBe("value set on static setter named async");
        }
    }

    expect("async" in A).toBeTrue();
    A.async = "value set on static setter named async";
    expect(called).toBeTrue();
});
