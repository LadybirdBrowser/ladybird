test("Issue #3548, exception in property getter with replacer function", () => {
    const o = {
        get foo() {
            throw Error();
        },
    };
    expect(() => {
        JSON.stringify(o, (_, value) => value);
    }).toThrow(Error);
});
