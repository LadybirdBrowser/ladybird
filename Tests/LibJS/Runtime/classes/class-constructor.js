test("property initialization", () => {
    class A {
        constructor() {
            this.x = 3;
        }
    }

    expect(new A().x).toBe(3);
});

test("method initialization", () => {
    class A {
        constructor() {
            this.x = () => 10;
        }
    }

    expect(new A().x()).toBe(10);
});

test("initialize to class method", () => {
    class A {
        constructor() {
            this.x = this.method;
        }

        method() {
            return 10;
        }
    }

    expect(new A().x()).toBe(10);
});

test("constructor length affects class length", () => {
    class A {
        constructor() {}
    }

    expect(A).toHaveLength(0);

    class B {
        constructor(a, b, c = 2) {}
    }

    expect(B).toHaveLength(2);
});

test("must be invoked with 'new'", () => {
    class A {
        constructor() {}
    }

    expect(() => {
        A();
    }).toThrowWithMessage(TypeError, "Class constructor A must be called with 'new'");

    expect(() => {
        A.prototype.constructor();
    }).toThrowWithMessage(TypeError, "Class constructor A must be called with 'new'");
});

test("implicit constructor", () => {
    class A {}

    expect(new A()).toBeInstanceOf(A);
});

test("can call constructor without parentheses", () => {
    class A {}

    // prettier-ignore
    expect(new A).toBeInstanceOf(A);
});

test("a string literal key whose value is constructor names the constructor", () => {
    class A {
        "construct\u006fr"() {
            this.x = 1;
        }
    }
    expect(new A().x).toBe(1);
    expect(A.prototype.constructor).toBe(A);

    class B extends A {
        "\u0063onstructor"() {
            super();
            this.y = 2;
        }
    }
    const b = new B();
    expect([b.x, b.y]).toEqual([1, 2]);

    expect(`class C { "construct\\u006fr"() {} constructor() {} }`).not.toEval();
    expect(`class C { get "construct\\u006fr"() {} }`).not.toEval();
    expect(`class C { "construct\\u006fr" = 1; }`).not.toEval();
    expect(`class C { static "prot\\u006ftype"() {} }`).not.toEval();
    expect(`class C { static "prot\\u006ftype" = 1; }`).not.toEval();
});
