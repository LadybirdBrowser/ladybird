test("basic functionality", () => {
    class A {
        get x() {
            return this._x;
        }

        set x(value) {
            this._x = value * 2;
        }
    }

    expect(A.x).toBeUndefined();
    expect(A).not.toHaveProperty("_x");
    const a = new A();
    expect(a.x).toBeUndefined();
    expect(a).not.toHaveProperty("_x");
    a.x = 3;
    expect(a.x).toBe(6);
    expect(a).toHaveProperty("_x", 6);
});

test("name", () => {
    class A {
        set x(v) {}
    }

    const d = Object.getOwnPropertyDescriptor(A.prototype, "x");
    expect(d.set.name).toBe("set x");
});

test("extended name syntax", () => {
    const s = Symbol("foo");

    class A {
        set "method with space"(value) {
            this.a = value;
        }

        set 12(value) {
            this.b = value;
        }

        set [`he${"llo"}`](value) {
            this.c = value;
        }

        set [s](value) {
            this.d = value;
        }
    }

    const a = new A();
    a["method with space"] = 1;
    a[12] = 2;
    a.hello = 3;
    a[s] = 4;
    expect(a.a).toBe(1);
    expect(a.b).toBe(2);
    expect(a.c).toBe(3);
    expect(a.d).toBe(4);
});

test("inherited setter", () => {
    class Parent {
        get x() {
            return this._x;
        }

        set x(value) {
            this._x = value * 2;
        }
    }

    class Child extends Parent {}

    const c = new Child();

    expect(c.x).toBeUndefined();
    c.x = 10;
    expect(c.x).toBe(20);
});

test("inherited static setter overriding", () => {
    class Parent {
        get x() {
            return this._x;
        }

        set x(value) {
            this._x = value * 2;
        }
    }

    class Child extends Parent {
        get x() {
            return this._x;
        }

        set x(value) {
            this._x = value * 3;
        }
    }

    const c = new Child();
    expect(c.x).toBeUndefined();
    c.x = 10;
    expect(c.x).toBe(30);
});

test("static setter named 'async'", () => {
    let called = false;
    class A {
        set async(value) {
            called = true;
            expect(value).toBe("value set on setter named async");
        }
    }

    const a = new A();
    expect("async" in a).toBeTrue();
    a.async = "value set on setter named async";
    expect(called).toBeTrue();
});
