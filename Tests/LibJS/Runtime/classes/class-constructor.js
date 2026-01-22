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
