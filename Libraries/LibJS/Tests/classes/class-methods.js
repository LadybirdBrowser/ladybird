test("basic functionality", () => {
    class A {
        number() {
            return 2;
        }

        string() {
            return "foo";
        }
    }

    const a = new A();
    expect(a.number()).toBe(2);
    expect(a.string()).toBe("foo");
});

test("length", () => {
    class A {
        method1() {}

        method2(a, b, c, d) {}

        method3(a, b, ...c) {}
    }

    const a = new A();
    expect(a.method1).toHaveLength(0);
    expect(a.method2).toHaveLength(4);
    expect(a.method3).toHaveLength(2);
});

test("extended name syntax", () => {
    class A {
        "method with space"() {
            return 1;
        }

        12() {
            return 2;
        }

        [`he${"llo"}`]() {
            return 3;
        }
    }

    const a = new A();
    expect(a["method with space"]()).toBe(1);
    expect(a[12]()).toBe(2);
    expect(a.hello()).toBe(3);
});

test("method named 'async'", () => {
    class A {
        async() {
            return "function named async";
        }
    }

    const a = new A();
    expect("async" in a).toBeTrue();
    expect(a.async()).toBe("function named async");
});

test("can call other private methods from methods", () => {
    class A {
        #a() {
            return 1;
        }

        async #b() {
            return 2;
        }

        syncA() {
            return this.#a();
        }

        async asyncA() {
            return this.#a();
        }

        syncB() {
            return this.#b();
        }

        async asyncB() {
            return this.#b();
        }
    }

    var called = false;

    async function check() {
        called = true;
        const a = new A();

        expect(a.syncA()).toBe(1);
        expect(await a.asyncA()).toBe(1);
        expect(await a.syncB()).toBe(2);
        expect(await a.asyncB()).toBe(2);
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
