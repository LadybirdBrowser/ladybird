test("basic functionality", () => {
    class A {
        accessor x = 1;
    }

    const a = new A();
    expect(a.x).toBe(1);

    a.x = 2;
    expect(a.x).toBe(2);
});

test("without initializer", () => {
    class A {
        accessor x;
    }

    const a = new A();
    expect(a.x).toBeUndefined();

    a.x = 42;
    expect(a.x).toBe(42);
});

test("multiple auto-accessors", () => {
    class A {
        accessor x = 1;
        accessor y = 2;
        accessor z = 3;
    }

    const a = new A();
    expect(a.x).toBe(1);
    expect(a.y).toBe(2);
    expect(a.z).toBe(3);

    a.x = 10;
    a.y = 20;
    a.z = 30;
    expect(a.x).toBe(10);
    expect(a.y).toBe(20);
    expect(a.z).toBe(30);
});

test("private auto-accessor", () => {
    class A {
        accessor #x = 1;

        getX() {
            return this.#x;
        }

        setX(v) {
            this.#x = v;
        }
    }

    const a = new A();
    expect(a.getX()).toBe(1);
    expect(a.x).toBeUndefined();

    a.setX(42);
    expect(a.getX()).toBe(42);
});

test("static auto-accessor", () => {
    class A {
        static accessor x = 1;
    }

    expect(A.x).toBe(1);

    A.x = 2;
    expect(A.x).toBe(2);

    expect(new A().x).toBeUndefined();
});

test("static private auto-accessor", () => {
    class A {
        static accessor #x = 1;

        static getX() {
            return this.#x;
        }

        static setX(v) {
            this.#x = v;
        }
    }

    expect(A.getX()).toBe(1);

    A.setX(42);
    expect(A.getX()).toBe(42);
});

// Assisted-By: claude opus 4.6 1m - originally checked new A() instead of
// A.prototype. Per spec, public auto-accessor getter/setter are defined on
// the prototype (homeObject) during ClassFieldDefinitionEvaluation, not on
// the instance. The instance only receives the private backing storage field.
// NB: spec says [[Enumerable]]: true, contradicting SpiderMonkey's test which
// asserts enumerable: false (gecko-dev accessors.js line 8).
test("property descriptor", () => {
    class A {
        accessor x = 1;
    }

    const desc = Object.getOwnPropertyDescriptor(A.prototype, "x");
    expect(typeof desc.get).toBe("function");
    expect(typeof desc.set).toBe("function");
    expect(desc.enumerable).toBeTrue();
    expect(desc.configurable).toBeTrue();
    expect(desc.value).toBeUndefined();
    expect(desc.writable).toBeUndefined();
});

test("static property descriptor", () => {
    class A {
        static accessor x = 1;
    }

    const desc = Object.getOwnPropertyDescriptor(A, "x");
    expect(typeof desc.get).toBe("function");
    expect(typeof desc.set).toBe("function");
    expect(desc.enumerable).toBeTrue();
    expect(desc.configurable).toBeTrue();
});

test("extended name syntax", () => {
    const s = Symbol("foo");

    class A {
        accessor "field with space" = 1;

        accessor 12 = "twelve";

        accessor [`he${"llo"}`] = 3;

        accessor [s] = 4;
    }

    const a = new A();
    expect(a["field with space"]).toBe(1);
    expect(a[12]).toBe("twelve");
    expect(a.hello).toBe(3);
    expect(a[s]).toBe(4);

    a["field with space"] = 10;
    expect(a["field with space"]).toBe(10);
});

test("initializer has correct this value", () => {
    class A {
        accessor x = this;
    }

    const a = new A();
    expect(a.x).toBe(a);
});

test("interleaving with regular fields", () => {
    const order = [];

    class A {
        x = (order.push("field x"), 1);
        accessor y = (order.push("accessor y"), 2);
        z = (order.push("field z"), 3);
    }

    const a = new A();
    expect(a.x).toBe(1);
    expect(a.y).toBe(2);
    expect(a.z).toBe(3);
    expect(order).toEqual(["field x", "accessor y", "field z"]);
});

test("each instance gets its own backing storage", () => {
    class A {
        accessor x = 1;
    }

    const a1 = new A();
    const a2 = new A();
    a1.x = 42;
    expect(a1.x).toBe(42);
    expect(a2.x).toBe(1);
});

test("inheritance", () => {
    class Parent {
        accessor x = 1;
    }

    class Child extends Parent {}

    const c = new Child();
    expect(c.x).toBe(1);

    c.x = 2;
    expect(c.x).toBe(2);
});

test("overriding in subclass", () => {
    class Parent {
        accessor x = 1;
    }

    class Child extends Parent {
        accessor x = 2;
    }

    const c = new Child();
    expect(c.x).toBe(2);
});

test("private auto-accessor brand check", () => {
    class A {
        accessor #x = 1;

        hasX(obj) {
            return #x in obj;
        }
    }

    const a = new A();
    expect(a.hasX(a)).toBeTrue();
    expect(a.hasX({})).toBeFalse();
});

test("private auto-accessor access from outside throws", () => {
    expect("class A { accessor #x = 1; } new A().#x").not.toEval();
});

describe("'accessor' as contextual keyword", () => {
    test("accessor as field name", () => {
        class A {
            accessor = 1;
        }

        const a = new A();
        expect(a.accessor).toBe(1);
    });

    test("accessor as method name", () => {
        class A {
            accessor() {
                return 42;
            }
        }

        expect(new A().accessor()).toBe(42);
    });

    test("accessor as getter name", () => {
        class A {
            get accessor() {
                return 42;
            }
        }

        expect(new A().accessor).toBe(42);
    });

    test("accessor as setter name", () => {
        class A {
            set accessor(v) {
                this._v = v;
            }
        }

        const a = new A();
        a.accessor = 42;
        expect(a._v).toBe(42);
    });

    test("static accessor as field name", () => {
        class A {
            static accessor = 1;
        }

        expect(A.accessor).toBe(1);
    });
});

describe("syntax errors", () => {
    test("accessor with generator is syntax error", () => {
        expect("class A { accessor *x() {} }").not.toEval();
    });

    test("accessor with async is syntax error", () => {
        expect("class A { accessor async x = 1; }").not.toEval();
    });

    test("accessor with get is syntax error", () => {
        expect("class A { accessor get x() {} }").not.toEval();
    });

    test("accessor with set is syntax error", () => {
        expect("class A { accessor set x(v) {} }").not.toEval();
    });

    test("duplicate private auto-accessor names", () => {
        expect("class A { accessor #x; accessor #x; }").not.toEval();
    });

    test("duplicate private auto-accessor and field", () => {
        expect("class A { accessor #x; #x; }").not.toEval();
        expect("class A { #x; accessor #x; }").not.toEval();
    });

    test("duplicate private static and instance auto-accessor", () => {
        expect("class A { static accessor #x; accessor #x; }").not.toEval();
    });

    test("accessor with parenthesized body is not valid", () => {
        expect("class A { accessor x() {} }").not.toEval();
    });
});

test("literal field initializers", () => {
    class A {
        accessor pos_int = 42;
        accessor neg_int = -1;
        accessor bool_val = true;
        accessor null_val = null;
        accessor str_val = "hello";
    }

    const a = new A();
    expect(a.pos_int).toBe(42);
    expect(a.neg_int).toBe(-1);
    expect(a.bool_val).toBeTrue();
    expect(a.null_val).toBeNull();
    expect(a.str_val).toBe("hello");
});
