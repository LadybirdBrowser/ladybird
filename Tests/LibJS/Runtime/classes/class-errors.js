describe("non-syntax errors", () => {
    test("super reference inside nested-but-same |this| scope with no base class", () => {
        expect(`
    class A {
      foo() {
        () => { super.bar; }
      }
    }`).toEval();
    });

    test("super reference property lookup with no base class", () => {
        expect(`
    class A {
      constructor() {
        super.foo;
      }
    }`).toEval();
    });
});

describe("reference errors", () => {
    test("derived class doesn't call super in constructor before using this", () => {
        class Parent {}
        class Child extends Parent {
            constructor() {
                this;
            }
        }

        expect(() => {
            new Child();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("derived class calls super twice in constructor", () => {
        class Parent {}
        class Child extends Parent {
            constructor() {
                super();
                super();
            }
        }

        expect(() => {
            new Child();
        }).toThrowWithMessage(ReferenceError, "|this| is already initialized");
    });
});

describe("syntax errors", () => {
    test("getter with argument", () => {
        expect(`
    class A {
      get foo(v) {
        return 0;
      }
    }`).not.toEval();
    });

    test("setter with no arguments", () => {
        expect(`
    class A {
      set foo() {
      }
    }`).not.toEval();
    });

    test("setter with more than one argument", () => {
        expect(`
    class A {
      set foo(bar, baz) {
      }
    }`).not.toEval();
        expect(`
    class A {
      set foo(...bar) {
      }
    }`).not.toEval();
    });

    test("super reference inside different |this| scope", () => {
        expect(`
    class Parent {}

    class Child extends Parent {
      foo() {
        function f() { super.foo; }
      }
    }`).not.toEval();
    });

    test("super reference call with no base class", () => {
        expect(`
    class A {
      constructor() {
        super();
      }
    }`).not.toEval();
    });
});
