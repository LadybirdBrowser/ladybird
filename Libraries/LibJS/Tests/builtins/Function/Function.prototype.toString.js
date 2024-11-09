describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Function.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        expect(function () {}.toString()).toBe("function () {}");
        expect(function (foo) {}.toString()).toBe("function (foo) {}");
        expect(function (foo, bar, baz) {}.toString()).toBe("function (foo, bar, baz) {}");
        // prettier-ignore
        expect((/* comment 1 */ function () { /* comment 2 */ } /* comment 3 */).toString()).toBe("function () { /* comment 2 */ }");
        expect(function* () {}.toString()).toBe("function* () {}");
        expect(async function () {}.toString()).toBe("async function () {}");
        expect(async function* () {}.toString()).toBe("async function* () {}");
        expect(
            function (foo, bar, baz) {
                if (foo) {
                    return baz;
                } else if (bar) {
                    return foo;
                }
                return bar + 42;
            }.toString()
        ).toBe(
            `function (foo, bar, baz) {
                if (foo) {
                    return baz;
                } else if (bar) {
                    return foo;
                }
                return bar + 42;
            }`
        );
    });

    test("object method", () => {
        expect({ foo() {} }.foo.toString()).toBe("foo() {}");
        expect({ ["foo"]() {} }.foo.toString()).toBe('["foo"]() {}');
        expect({ *foo() {} }.foo.toString()).toBe("*foo() {}");
        expect({ async foo() {} }.foo.toString()).toBe("async foo() {}");
        expect({ async *foo() {} }.foo.toString()).toBe("async *foo() {}");
        expect(Object.getOwnPropertyDescriptor({ get foo() {} }, "foo").get.toString()).toBe(
            "get foo() {}"
        );
        expect(Object.getOwnPropertyDescriptor({ set foo(x) {} }, "foo").set.toString()).toBe(
            "set foo(x) {}"
        );
    });

    test("arrow function", () => {
        expect((() => {}).toString()).toBe("() => {}");
        expect((foo => {}).toString()).toBe("foo => {}");
        // prettier-ignore
        expect(((foo) => {}).toString()).toBe("(foo) => {}");
        expect(((foo, bar) => {}).toString()).toBe("(foo, bar) => {}");
        expect((() => foo).toString()).toBe("() => foo");
        // prettier-ignore
        expect((() => { /* comment */ }).toString()).toBe("() => { /* comment */ }");
    });

    test("class expression", () => {
        expect(class {}.toString()).toBe("class {}");
        expect(class Foo {}.toString()).toBe("class Foo {}");
        // prettier-ignore
        expect(class Foo { bar() {} }.toString()).toBe("class Foo { bar() {} }");
        // prettier-ignore
        expect((/* comment 1 */ class { /* comment 2 */ } /* comment 3 */).toString()).toBe("class { /* comment 2 */ }");

        class Bar {}
        expect(
            class Foo extends Bar {
                constructor() {
                    super();
                }

                a = 1;
                #b = 2;
                static c = 3;

                /* comment */

                async *foo() {
                    return 42;
                }
            }.toString()
        ).toBe(
            `class Foo extends Bar {
                constructor() {
                    super();
                }

                a = 1;
                #b = 2;
                static c = 3;

                /* comment */

                async *foo() {
                    return 42;
                }
            }`
        );
    });

    test("class constructor", () => {
        expect(class {}.constructor.toString()).toBe("function Function() { [native code] }");
        // prettier-ignore
        expect(class { constructor() {} }.constructor.toString()).toBe("function Function() { [native code] }");
    });

    // prettier-ignore
    test("class method", () => {
        expect(new (class { foo() {} })().foo.toString()).toBe("foo() {}");
        expect(new (class { ["foo"]() {} })().foo.toString()).toBe('["foo"]() {}');
    });

    // prettier-ignore
    test("static class method", () => {
        expect(class { static foo() {} }.foo.toString()).toBe("foo() {}");
        expect(class { static ["foo"]() {} }.foo.toString()).toBe('["foo"]() {}');
        expect(class { static *foo() {} }.foo.toString()).toBe("*foo() {}");
        expect(class { static async foo() {} }.foo.toString()).toBe("async foo() {}");
        expect(class { static async *foo() {} }.foo.toString()).toBe("async *foo() {}");
    });

    test("native function", () => {
        // Built-in functions
        expect(console.debug.toString()).toBe("function debug() { [native code] }");
        expect(Function.toString()).toBe("function Function() { [native code] }");
        expect(
            Object.getOwnPropertyDescriptor(Temporal.TimeZone.prototype, "id").get.toString()
        ).toBe("function get id() { [native code] }");

        const values = [
            // Callable Proxy
            new Proxy(function foo() {}, {}),
            // Bound function
            function foo() {}.bind(null),
            // Wrapped function
            new ShadowRealm().evaluate("function foo() {}; foo"),
        ];
        for (const fn of values) {
            // Inner function name is not exposed
            expect(fn.toString()).toBe("function () { [native code] }");
        }
    });
});
