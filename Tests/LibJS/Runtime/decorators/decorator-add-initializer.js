// addInitializer tests.

test("method decorator addInitializer runs on instantiation", () => {
    const log = [];
    function dec(method, context) {
        context.addInitializer(function () {
            log.push(`init ${context.name} on ${this.constructor.name}`);
        });
    }

    class A {
        @dec
        method() {}
    }

    expect(log).toEqual([]);
    new A();
    expect(log).toEqual(["init method on A"]);
});

test("field decorator addInitializer runs after field init", () => {
    const log = [];
    function dec(value, context) {
        context.addInitializer(function () {
            log.push("extra init");
        });
    }

    class A {
        @dec
        x = (log.push("field init"), 1);
    }

    new A();
    expect(log).toEqual(["field init", "extra init"]);
});

test("class decorator addInitializer runs after class definition", () => {
    const log = [];
    function dec(cls, context) {
        context.addInitializer(function () {
            log.push("class init");
        });
    }

    expect(log).toEqual([]);

    @dec
    class A {}

    expect(log).toEqual(["class init"]);
});

test("addInitializer throws if called after decoration finishes", () => {
    let savedAddInit;
    function dec(value, context) {
        savedAddInit = context.addInitializer;
    }

    class A {
        @dec
        method() {}
    }

    expect(() => {
        savedAddInit(function () {});
    }).toThrowWithMessage(TypeError, "addInitializer must not be called after decoration has finished");
});

test("addInitializer throws for non-callable argument", () => {
    function dec(value, context) {
        context.addInitializer(42);
    }

    expect(() => {
        class A {
            @dec
            method() {}
        }
    }).toThrowWithMessage(TypeError, "Argument to addInitializer must be a function");
});
