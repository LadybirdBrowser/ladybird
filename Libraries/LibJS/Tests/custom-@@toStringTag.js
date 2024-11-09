test("inside objects", () => {
    const o = {
        [Symbol.toStringTag]: "hello friends",
    };

    expect(o.toString()).toBe("[object hello friends]");
});

test("inside classes", () => {
    class A {
        constructor() {
            this[Symbol.toStringTag] = "hello friends";
        }
    }

    const a = new A();
    expect(a.toString()).toBe("[object hello friends]");
});

test("non-string values are ignored", () => {
    const o = {
        [Symbol.toStringTag]: [1, 2, 3],
    };

    expect(o.toString()).toBe("[object Object]");
});
