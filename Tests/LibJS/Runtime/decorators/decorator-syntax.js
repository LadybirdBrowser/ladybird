// Decorator syntax parsing tests.
// Phase 2: parsing only -- decorators are no-ops at this stage.
// A no-op decorator that does nothing (returns undefined).
function dec() {}
function dec1() {}
function dec2() {}

const decorators = {
    dec() {},
    nested: {
        dec() {},
    },
};

describe("class element decorators", () => {
    test("method decorator", () => {
        class A {
            @dec method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("field decorator", () => {
        class A {
            @dec x = 1;
        }
        expect(new A().x).toBe(1);
    });

    test("auto-accessor decorator", () => {
        class A {
            @dec accessor x = 1;
        }
        expect(new A().x).toBe(1);
    });

    test("getter decorator", () => {
        class A {
            @dec get x() {
                return 1;
            }
        }
        expect(new A().x).toBe(1);
    });

    test("setter decorator", () => {
        class A {
            @dec set x(v) {
                this._v = v;
            }
        }
        const a = new A();
        a.x = 42;
        expect(a._v).toBe(42);
    });

    test("static method decorator", () => {
        class A {
            @dec static method() {
                return 1;
            }
        }
        expect(A.method()).toBe(1);
    });

    test("static field decorator", () => {
        class A {
            @dec static x = 1;
        }
        expect(A.x).toBe(1);
    });

    test("multiple decorators on one element", () => {
        class A {
            @dec1
            @dec2
            method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });
});

describe("class decorators", () => {
    test("class declaration decorator", () => {
        @dec
        class A {
            x = 1;
        }
        expect(new A().x).toBe(1);
    });

    test("multiple class decorators", () => {
        @dec1
        @dec2
        class A {}
        expect(new A()).toBeInstanceOf(A);
    });

    test("class expression decorator", () => {
        const A =
            @dec
            class {
                x = 1;
            };
        expect(new A().x).toBe(1);
    });
});

describe("decorator expression forms", () => {
    test("identifier decorator", () => {
        class A {
            @dec method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("member expression decorator", () => {
        class A {
            @decorators.dec method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("nested member expression decorator", () => {
        class A {
            @decorators.nested.dec method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("call expression decorator", () => {
        function factory() {
            return function () {};
        }
        class A {
            @factory() method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("member call expression decorator", () => {
        const obj = {
            factory() {
                return function () {};
            },
        };
        class A {
            @obj.factory() method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("parenthesized expression decorator", () => {
        class A {
            @(dec) method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });

    test("parenthesized complex expression decorator", () => {
        const decs = [dec];
        class A {
            @(decs[0]) method() {
                return 1;
            }
        }
        expect(new A().method()).toBe(1);
    });
});

describe("syntax errors", () => {
    test("decorator on constructor is syntax error", () => {
        expect("class A { @dec constructor() {} }").not.toEval();
    });

    test("decorator on static block is syntax error", () => {
        expect("class A { @dec static {} }").not.toEval();
    });

    test("decorator on non-class declaration is syntax error", () => {
        expect("@dec function f() {}").not.toEval();
        expect("@dec let x;").not.toEval();
        expect("@dec const x = 1;").not.toEval();
        expect("@dec var x;").not.toEval();
    });

    test("bare @ is syntax error", () => {
        expect("class A { @ method() {} }").not.toEval();
    });

    test("decorator with computed access is syntax error", () => {
        expect("class A { @dec[0] method() {} }").not.toEval();
    });

    test("decorator with optional chaining is syntax error", () => {
        expect("class A { @dec?.foo method() {} }").not.toEval();
    });

    test("decorator with template literal is syntax error", () => {
        expect("class A { @dec`str` method() {} }").not.toEval();
    });
});
