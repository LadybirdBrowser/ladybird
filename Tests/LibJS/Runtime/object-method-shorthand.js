test("basic method shorthand", () => {
    const o = {
        foo: "bar",
        getFoo() {
            return this.foo;
        },
    };
    expect(o.getFoo()).toBe("bar");
});

test("numeric literal method shorthand", () => {
    const o = {
        foo: "bar",
        12() {
            return this.foo;
        },
    };
    expect(o[12]()).toBe("bar");
});

test("string literal method shorthand", () => {
    const o = {
        foo: "bar",
        "hello friends"() {
            return this.foo;
        },
    };
    expect(o["hello friends"]()).toBe("bar");
});

test("computed property method shorthand", () => {
    const o = {
        foo: "bar",
        [4 + 10]() {
            return this.foo;
        },
    };
    expect(o[14]()).toBe("bar");
});

test("symbol computed property shorthand", () => {
    const s = Symbol("foo");
    const o = {
        foo: "bar",
        [s]() {
            return this.foo;
        },
    };
    expect(o[s]()).toBe("bar");
});
