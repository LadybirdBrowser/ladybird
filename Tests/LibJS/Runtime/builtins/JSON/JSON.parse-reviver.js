test("basic functionality", () => {
    let string = `{"var1":10,"var2":"hello","var3":{"nested":5}}`;

    let object = JSON.parse(string, (key, value) => (typeof value === "number" ? value * 2 : value));
    expect(object).toEqual({ var1: 20, var2: "hello", var3: { nested: 10 } });

    object = JSON.parse(string, (key, value) => (typeof value === "number" ? undefined : value));
    expect(object).toEqual({ var2: "hello", var3: {} });
});

test("reviver context exposes source text of primitive values", () => {
    const sources = [];
    JSON.parse('[1.5, "a", true, null, {"x": 2}]', function (key, value, context) {
        expect(typeof context).toBe("object");
        expect(Object.getPrototypeOf(context)).toBe(Object.prototype);
        sources.push(context.source);
        return value;
    });
    // Primitives carry their matched source text; arrays/objects have no source.
    expect(sources).toEqual(["1.5", '"a"', "true", "null", "2", undefined, undefined]);
});

test("reviver context has no source for forward-modified values", () => {
    const result = JSON.parse("[1, 2]", function (key, value, { source }) {
        if (key === "0") this[1] = { injected: true };
        else if (key !== "") expect(source).toBeUndefined();
        return this[key];
    });
    expect(result).toEqual([1, { injected: true }]);
});
