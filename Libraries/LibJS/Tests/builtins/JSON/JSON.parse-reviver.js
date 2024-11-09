test("basic functionality", () => {
    let string = `{"var1":10,"var2":"hello","var3":{"nested":5}}`;

    let object = JSON.parse(string, (key, value) =>
        typeof value === "number" ? value * 2 : value
    );
    expect(object).toEqual({ var1: 20, var2: "hello", var3: { nested: 10 } });

    object = JSON.parse(string, (key, value) => (typeof value === "number" ? undefined : value));
    expect(object).toEqual({ var2: "hello", var3: {} });
});
