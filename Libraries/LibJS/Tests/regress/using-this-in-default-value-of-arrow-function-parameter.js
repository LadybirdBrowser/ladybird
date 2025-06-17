test("using this in default value of arrow function parameter does not crash", () => {
    const result = [];

    class A {
        constructor() {
            this.foo = (bar = this.value1, baz = this.value2, value3 = this.value3) => {
                result.push(bar);
                result.push(baz);
                result.push(value3);
            };

            this.value1 = 20;
            this.value2 = 30;
            this.value3 = 40;
        }
    }

    new A().foo(10);

    expect(result).toEqual([10, 30, 40]);
});
