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
test("new-expressions with object keys", () => {
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
