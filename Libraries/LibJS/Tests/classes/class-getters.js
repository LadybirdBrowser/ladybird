test("basic functionality", () => {
    class A {
        get x() {
            return 10;
        }
    }

    expect(A).not.toHaveProperty("x");
    expect(new A().x).toBe(10);
});
test("name", () => {
    class A {
        get x() {}
    }

    const d = Object.getOwnPropertyDescriptor(A.prototype, "x");
    expect(d.get.name).toBe("get x");
});

test("extended name syntax", () => {
    const s = Symbol("foo");

    class A {
        get "method with space"() {
            return 1;
        }

        get 12() {
            return 2;
        }

        get [`he${"llo"}`]() {
            return 3;
        }

        get [s]() {
            return 4;
        }
    }

    const a = new A();
    expect(a["method with space"]).toBe(1);
    expect(a[12]).toBe(2);
    expect(a.hello).toBe(3);
    expect(a[s]).toBe(4);
});

test("inherited getter", () => {
    class Parent {
        get x() {
            return 3;
        }
    }

    class Child extends Parent {}

    expect(Child).not.toHaveProperty("x");
    expect(new Child().x).toBe(3);
});

test("inherited getter overriding", () => {
    class Parent {
        get x() {
            return 3;
        }
    }

    class Child extends Parent {
        get x() {
            return 10;
        }
    }

    expect(new Child().x).toBe(10);
});

test("static getter named 'async'", () => {
    class A {
        get async() {
            return "getter named async";
        }
    }

    const a = new A();
    expect("async" in a).toBeTrue();
    expect(a.async).toBe("getter named async");
});
