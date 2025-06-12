test("basic functionality", () => {
    const sum = (a, b, c) => a + b + c;
    const a = [1, 2, 3];

    expect(sum(...a)).toBe(6);
    expect(sum(1, ...a)).toBe(4);
    expect(sum(...a, 10)).toBe(6);

    const foo = (a, b, c) => c;

    const o = { bar: [1, 2, 3] };
    expect(foo(...o.bar)).toBe(3);
    expect(foo(..."abc")).toBe("c");
});

test("spreading custom iterable", () => {
    let o = {
        [Symbol.iterator]() {
            return {
                i: 0,
                next() {
                    if (this.i++ === 3) {
                        return { done: true };
                    }
                    return { value: this.i };
                },
            };
        },
    };

    expect(Math.max(...o)).toBe(3);
});

test("spreading non iterable", () => {
    expect(() => {
        [...1];
    }).toThrowWithMessage(TypeError, "1 is not iterable");
});
