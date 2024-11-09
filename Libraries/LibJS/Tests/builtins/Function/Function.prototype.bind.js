describe("basic behavior", () => {
    test("basic binding", () => {
        expect(Function.prototype.bind).toHaveLength(1);

        var charAt = String.prototype.charAt.bind("bar");
        expect(charAt(0) + charAt(1) + charAt(2)).toBe("bar");

        function getB() {
            return this.toUpperCase().charAt(0);
        }
        expect(getB.bind("bar")()).toBe("B");
    });

    test("bound functions work with array functions", () => {
        var Make3 = Number.bind(null, 3);
        expect([55].map(Make3)[0]).toBe(3);

        var MakeTrue = Boolean.bind(null, true);

        expect([1, 2, 3].filter(MakeTrue)).toHaveLength(3);
        expect(
            [1, 2, 3].reduce(
                function (acc, x) {
                    return acc + x;
                }.bind(null, 4, 5)
            )
        ).toBe(9);
        expect(
            [1, 2, 3].reduce(
                function (acc, x) {
                    return acc + x + this;
                }.bind(3)
            )
        ).toBe(12);
    });

    test("name has 'bound' prefix", () => {
        function foo() {}
        const boundFoo = foo.bind(123);
        expect(foo.name).toBe("foo");
        expect(boundFoo.name).toBe("bound foo");
    });

    test("prototype is inherited from target function", () => {
        function foo() {}
        Object.setPrototypeOf(foo, Array.prototype);
        const boundFoo = Function.prototype.bind.call(foo, 123);
        expect(Object.getPrototypeOf(boundFoo)).toBe(Array.prototype);
    });
});

describe("bound function arguments", () => {
    function sum(a, b, c) {
        return a + b + c;
    }
    var boundSum = sum.bind(null, 10, 5);

    test("arguments are bound to the function", () => {
        expect(boundSum()).toBeNaN();
        expect(boundSum(5)).toBe(20);
        expect(boundSum(5, 6, 7)).toBe(20);
    });

    test("arguments are appended to a BoundFunction's bound arguments", () => {
        expect(boundSum.bind(null, 5)()).toBe(20);
    });

    test("binding a constructor's arguments", () => {
        var Make5 = Number.bind(null, 5);
        expect(Make5()).toBe(5);
        expect(new Make5().valueOf()).toBe(5);
    });

    test("length property", () => {
        expect(sum).toHaveLength(3);
        expect(boundSum).toHaveLength(1);
        expect(boundSum.bind(null, 5)).toHaveLength(0);
        expect(boundSum.bind(null, 5, 6, 7, 8)).toHaveLength(0);
    });
});

describe("bound function |this|", () => {
    function identity() {
        return this;
    }

    test("captures global object as |this| if |this| is null or undefined", () => {
        expect(identity.bind()()).toBe(globalThis);
        expect(identity.bind(null)()).toBe(globalThis);
        expect(identity.bind(undefined)()).toBe(globalThis);

        function Foo() {
            expect(identity.bind()()).toBe(globalThis);
            expect(identity.bind(this)()).toBe(this);
        }
        new Foo();
    });

    test("does not capture global object as |this| if |this| is null or undefined in strict mode", () => {
        "use strict";

        function strictIdentity() {
            return this;
        }

        expect(strictIdentity.bind()()).toBeUndefined();
        expect(strictIdentity.bind(null)()).toBeNull();
        expect(strictIdentity.bind(undefined)()).toBeUndefined();
    });

    test("primitive |this| values are converted to objects", () => {
        expect(identity.bind("foo")()).toBeInstanceOf(String);
        expect(identity.bind(123)()).toBeInstanceOf(Number);
        expect(identity.bind(true)()).toBeInstanceOf(Boolean);
    });

    test("bound functions retain |this| values passed to them", () => {
        var obj = { foo: "bar" };
        expect(identity.bind(obj)()).toBe(obj);
    });

    test("bound |this| cannot be changed after being set", () => {
        expect(identity.bind("foo").bind(123)()).toBeInstanceOf(String);
    });

    test("arrow functions cannot be bound", () => {
        expect((() => this).bind("foo")()).toBe(globalThis);
    });

    test("length of original function is used for bound function", () => {
        [0, 1, 2147483647, 2147483648, 2147483649].forEach(value => {
            function emptyFunction() {}

            Object.defineProperty(emptyFunction, "length", { value });

            expect(emptyFunction.bind().length).toBe(value);
            expect(emptyFunction.bind(null).length).toBe(value);
            expect(emptyFunction.bind(null, 0).length).toBe(Math.max(0, value - 1));
            expect(emptyFunction.bind(null, 0, 1, 2).length).toBe(Math.max(0, value - 3));
        });
    });
});

describe("bound function constructors", () => {
    function Bar() {
        this.x = 3;
        this.y = 4;
    }

    Bar.prototype.baz = "baz";
    var BoundBar = Bar.bind({ u: 5, v: 6 });
    var bar = new BoundBar();

    test("bound |this| value does not affect constructor", () => {
        expect(bar.x).toBe(3);
        expect(bar.y).toBe(4);
        expect(typeof bar.u).toBe("undefined");
        expect(typeof bar.v).toBe("undefined");
    });

    test("bound functions retain original prototype", () => {
        expect(bar.baz).toBe("baz");
    });

    test("bound functions do not have a prototype property", () => {
        expect(BoundBar).not.toHaveProperty("prototype");
    });
});

describe("errors", () => {
    test("does not accept non-function values", () => {
        expect(() => {
            Function.prototype.bind.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not a function");
    });
});
