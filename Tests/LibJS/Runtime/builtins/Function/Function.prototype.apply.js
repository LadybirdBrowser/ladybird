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

test("array with holes", () => {
    function target(a, b, c) {
        return a + b + c;
    }

    const args = [1, , 3];
    const result = target.apply(null, args);
    expect(result).toBe(NaN);
});

test("length getter that changes the array-like", () => {
    function target(...args) {
        return args;
    }

    const removesElement = {
        0: 1,
        get length() {
            delete this[0];
            return 2;
        },
    };
    expect(target.apply(null, removesElement)).toEqual([undefined, undefined]);

    const addsElements = {
        0: 1,
        get length() {
            this[1] = 2;
            this[2] = 3;
            return 3;
        },
    };
    expect(target.apply(null, addsElements)).toEqual([1, 2, 3]);
});

test("length is read once", () => {
    function target(...args) {
        return args;
    }

    let count = 0;
    const arrayLike = {
        0: 1,
        get length() {
            ++count;
            return 2;
        },
    };
    expect(target.apply(null, arrayLike)).toEqual([1, undefined]);
    expect(count).toBe(1);
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

test("array-likes whose elements are read through internal methods", () => {
    function collect(...values) {
        return values;
    }
    const arrayLike = {
        length: 3,
        get 0() {
            gc();
            return { first: true };
        },
        1: "second",
        get 2() {
            gc();
            return ["third"];
        },
    };
    expect(collect.apply(null, arrayLike)).toEqual([{ first: true }, "second", ["third"]]);

    const many = { length: 40 };
    for (let i = 0; i < 40; ++i) many[i] = i;
    expect(collect.apply(null, many)).toEqual(Array.from({ length: 40 }, (_, i) => i));

    function mapped(a, b, c) {
        a = "changed";
        return collect.apply(null, arguments);
    }
    expect(mapped(1, 2, 3)).toEqual(["changed", 2, 3]);
    expect(mapped(1)).toEqual(["changed"]);

    const throwing = {
        length: 2,
        0: 1,
        get 1() {
            throw new Error("from a getter");
        },
    };
    expect(() => collect.apply(null, throwing)).toThrowWithMessage(Error, "from a getter");
});
