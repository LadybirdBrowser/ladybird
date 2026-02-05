test("basic functionality", () => {
    class A {
        static method() {
            return 10;
        }
    }

    expect(A.method()).toBe(10);
    expect(new A().method).toBeUndefined();
});

test("extended name syntax", () => {
    class A {
        static method() {
            return 1;
        }

        static 12() {
            return 2;
        }

        static [`he${"llo"}`]() {
            return 3;
        }
    }

    expect(A.method()).toBe(1);
    expect(A[12]()).toBe(2);
    expect(A.hello()).toBe(3);
});

test("bound |this|", () => {
    class A {
        static method() {
            expect(this).toBe(A);
        }
    }

    A.method();
});

test("inherited static methods", () => {
    class Parent {
        static method() {
            return 3;
        }
    }

    class Child extends Parent {}

    expect(Parent.method()).toBe(3);
    expect(Child.method()).toBe(3);
    expect(new Parent()).not.toHaveProperty("method");
    expect(new Child()).not.toHaveProperty("method");
});

test("static method overriding", () => {
    class Parent {
        static method() {
            return 3;
        }
    }

    class Child extends Parent {
        static method() {
            return 10;
        }
    }

    expect(Parent.method()).toBe(3);
    expect(Child.method()).toBe(10);
});

test("static function named 'async'", () => {
    class A {
        static async() {
            return "static function named async";
        }
    }

    expect("async" in A).toBeTrue();
    expect(A.async()).toBe("static function named async");
});

test("can call other private methods from static method", () => {
    class A {
        static #a() {
            return 1;
        }

        static async #b() {
            return 2;
        }

        static syncA() {
            return this.#a();
        }

        static async asyncA() {
            return this.#a();
        }

        static syncB() {
            return this.#b();
        }

        static async asyncB() {
            return this.#b();
        }
    }

    var called = false;

    async function check() {
        called = true;
        expect(A.syncA()).toBe(1);
        expect(await A.asyncA()).toBe(1);
        expect(await A.syncB()).toBe(2);
        expect(await A.asyncB()).toBe(2);
        return 3;
    }

    var error = null;
    var failed = false;

    check().then(
        value => {
            expect(called).toBeTrue();
            expect(value).toBe(3);
        },
        thrownError => {
            failed = true;
            error = thrownError;
        }
    );

    runQueuedPromiseJobs();

    expect(called).toBeTrue();

    if (failed) throw error;

    expect(failed).toBeFalse();
});
