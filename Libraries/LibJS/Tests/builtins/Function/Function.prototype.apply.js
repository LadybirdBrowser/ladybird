test("basic functionality", () => {
    function Foo(arg) {
        this.foo = arg;
    }
    function Bar(arg) {
        this.bar = arg;
    }
    function FooBar(arg) {
        Foo.apply(this, [arg]);
        Bar.apply(this, [arg]);
    }
    function FooBarBaz(arg) {
        Foo.apply(this, [arg]);
        Bar.apply(this, [arg]);
        this.baz = arg;
    }

    expect(Function.prototype.apply).toHaveLength(2);

    var foo = new Foo("test");
    expect(foo.foo).toBe("test");
    expect(foo.bar).toBeUndefined();
    expect(foo.baz).toBeUndefined();

    var bar = new Bar("test");
    expect(bar.foo).toBeUndefined();
    expect(bar.bar).toBe("test");
    expect(bar.baz).toBeUndefined();

    var foobar = new FooBar("test");
    expect(foobar.foo).toBe("test");
    expect(foobar.bar).toBe("test");
    expect(foobar.baz).toBeUndefined();

    var foobarbaz = new FooBarBaz("test");
    expect(foobarbaz.foo).toBe("test");
    expect(foobarbaz.bar).toBe("test");
    expect(foobarbaz.baz).toBe("test");

    expect(Math.abs.apply(null, [-1])).toBe(1);

    var add = (x, y) => x + y;
    expect(add.apply(null, [1, 2])).toBe(3);

    var multiply = function (x, y) {
        return x * y;
    };
    expect(multiply.apply(null, [3, 4])).toBe(12);

    expect((() => this).apply("foo")).toBe(globalThis);
});

describe("errors", () => {
    test("does not accept non-function values", () => {
        expect(() => {
            Function.prototype.apply.call("foo");
        }).toThrowWithMessage(TypeError, "foo is not a function");

        expect(() => {
            Function.prototype.apply.call(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");

        expect(() => {
            Function.prototype.apply.call(null);
        }).toThrowWithMessage(TypeError, "null is not a function");
    });
});
