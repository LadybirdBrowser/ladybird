// Decorator evaluation and application order tests.

test("decorator expressions evaluated in source order", () => {
    const log = [];
    function track(id) {
        log.push(`eval ${id}`);
        return function () {};
    }

    class A {
        @track("m1")
        method1() {}

        @track("f1")
        field1 = 1;

        @track("m2")
        method2() {}
    }

    expect(log).toEqual(["eval m1", "eval f1", "eval m2"]);
});

test("decorators applied in spec order: static methods, instance methods, static fields, instance fields", () => {
    const log = [];
    function track(id) {
        return function (value, context) {
            log.push(`apply ${id} (${context.kind}, static=${context.static})`);
        };
    }

    class A {
        @track("instance-field")
        x = 1;

        @track("static-method")
        static sm() {}

        @track("instance-method")
        im() {}

        @track("static-field")
        static sf = 1;
    }

    // Decoration order per spec:
    // 1. static methods/accessors
    // 2. instance methods/accessors
    // 3. static fields
    // 4. instance fields
    expect(log).toEqual([
        "apply static-method (method, static=true)",
        "apply instance-method (method, static=false)",
        "apply static-field (field, static=true)",
        "apply instance-field (field, static=false)",
    ]);
});

test("class decorators applied after element decorators", () => {
    const log = [];
    function track(id) {
        return function () {
            log.push(id);
        };
    }

    @track("class")
    class A {
        @track("method")
        method() {}
    }

    // Element decorators before class decorators
    expect(log[0]).toBe("method");
    expect(log[1]).toBe("class");
});
