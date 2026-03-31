// Decorator context object tests.

describe("context.kind", () => {
    test("method context has kind 'method'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            method() {}
        }

        expect(ctx.kind).toBe("method");
    });

    test("getter context has kind 'getter'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            get x() {
                return 1;
            }
        }

        expect(ctx.kind).toBe("getter");
    });

    test("setter context has kind 'setter'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            set x(v) {}
        }

        expect(ctx.kind).toBe("setter");
    });

    test("field context has kind 'field'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            x = 1;
        }

        expect(ctx.kind).toBe("field");
    });

    test("auto-accessor context has kind 'accessor'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            accessor x = 1;
        }

        expect(ctx.kind).toBe("accessor");
    });

    test("class context has kind 'class'", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        @capture
        class A {}

        expect(ctx.kind).toBe("class");
    });
});

describe("context.name", () => {
    test("public method name", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            myMethod() {}
        }

        expect(ctx.name).toBe("myMethod");
    });

    test("public field name", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            myField = 1;
        }

        expect(ctx.name).toBe("myField");
    });

    test("private method name includes #", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            #myMethod() {}
        }

        expect(ctx.name).toBe("#myMethod");
    });

    test("class decorator name is class name", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        @capture
        class MyClass {}

        expect(ctx.name).toBe("MyClass");
    });
});

describe("context.static", () => {
    test("instance method is not static", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            method() {}
        }

        expect(ctx.static).toBeFalse();
    });

    test("static method is static", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            static method() {}
        }

        expect(ctx.static).toBeTrue();
    });

    test("class decorator has no static property", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        @capture
        class A {}

        expect(ctx).not.toHaveProperty("static");
    });
});

describe("context.private", () => {
    test("public method is not private", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            method() {}
        }

        expect(ctx.private).toBeFalse();
    });

    test("private method is private", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            #method() {}
        }

        expect(ctx.private).toBeTrue();
    });

    test("class decorator has no private property", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        @capture
        class A {}

        expect(ctx).not.toHaveProperty("private");
    });
});

describe("context.access", () => {
    test("method access has get and has", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            method() {
                return 42;
            }
        }

        const a = new A();
        expect(typeof ctx.access.get).toBe("function");
        expect(typeof ctx.access.has).toBe("function");
        expect(ctx.access.get(a)).toBe(a.method);
        expect(ctx.access.has(a)).toBeTrue();
    });

    test("field access has get, set, and has", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        class A {
            @capture
            x = 42;
        }

        const a = new A();
        expect(typeof ctx.access.get).toBe("function");
        expect(typeof ctx.access.set).toBe("function");
        expect(typeof ctx.access.has).toBe("function");
        expect(ctx.access.get(a)).toBe(42);
        ctx.access.set(a, 100);
        expect(a.x).toBe(100);
    });

    test("class decorator has no access property", () => {
        let ctx;
        function capture(v, context) {
            ctx = context;
        }

        @capture
        class A {}

        expect(ctx).not.toHaveProperty("access");
    });
});
