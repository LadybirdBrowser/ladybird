// prettier-ignore
test("new-expression parsing", () => {
    function Foo() {
        this.x = 1;
    }

    let foo = new Foo();
    expect(foo.x).toBe(1);

    foo = new Foo
    expect(foo.x).toBe(1);

    foo = new
    Foo
    ();
    expect(foo.x).toBe(1);

    foo = new Foo + 2
    expect(foo).toBe("[object Object]2");
});

// prettier-ignore
test("new on function-valued object properties", () => {
    let a = {
        b: function () {
            this.x = 2;
        },
    };

    foo = new a.b();
    expect(foo.x).toBe(2);

    foo = new a.b;
    expect(foo.x).toBe(2);

    foo = new
    a.b();
    expect(foo.x).toBe(2);
});

test("new-expressions with function calls", () => {
    function funcGetter() {
        return function (a, b) {
            this.x = a + b;
        };
    }

    foo = new funcGetter()(1, 5);
    expect(foo).toBeUndefined();

    foo = new (funcGetter())(1, 5);
    expect(foo.x).toBe(6);
});

// prettier-ignore
test("new on class instance method throws TypeError", () => {
    class FAIL {
        m() {}
    }
    const fail = new FAIL();

    expect(() => {
        new fail.m;
    }).toThrowWithMessage(TypeError, "");
});

// prettier-ignore
test("new on object literal method throws TypeError", () => {
    expect(() => {
        new ({ m() {} }).m;
    }).toThrowWithMessage(TypeError, "");
});

// prettier-ignore
test("new on extracted class method throws TypeError", () => {
    const m = class { m() {} }.prototype.m;

    expect(() => {
        new m;
    }).toThrowWithMessage(TypeError, "");
});

test("new on function expression property is allowed", () => {
    const obj = {
        b: function () {
            this.x = 2;
        },
    };

    const foo = new obj.b();
    expect(foo.x).toBe(2);
});
