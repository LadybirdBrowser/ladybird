test("Issue #3459, exception in computed property expression", () => {
    expect(() => {
        "foo"[bar];
    }).toThrow(ReferenceError);
    expect(() => {
        "foo"[bar]();
    }).toThrow(ReferenceError);
});

test("Issue #3941, exception in computed property's toString()", () => {
    expect(() => {
        const o = {
            toString() {
                throw Error();
            },
        };
        "foo"[o];
    }).toThrow(Error);
});
