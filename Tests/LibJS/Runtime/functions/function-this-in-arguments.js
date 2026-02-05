test("basic functionality", () => {
    expect(typeof this).toBe("object");
    expect(this).toBe(globalThis);
});

test("this inside instantiated functions is not globalThis", () => {
    let functionThis;
    function Foo() {
        this.x = 5;
        functionThis = this;
    }

    new Foo();
    expect(typeof functionThis).toBe("object");
    expect(functionThis.x).toBe(5);
});
