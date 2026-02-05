a = 1;
b = 42;

test("basic functionality", () => {
    expect(delete a).toBeTrue();

    expect(() => {
        a;
    }).toThrowWithMessage(ReferenceError, "'a' is not defined");
});

test("delete global var after usage", () => {
    let errors = 0;
    for (let i = 0; i < 3; ++i) {
        try {
            b++;
        } catch {
            ++errors;
        }
        delete globalThis.b;
    }
    expect(errors).toBe(2);
});
