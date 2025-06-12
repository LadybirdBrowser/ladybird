test("constructs properly", () => {
    expect(() => {
        new Proxy({}, {});
    }).not.toThrow();
});

test("constructor argument count", () => {
    expect(() => {
        new Proxy();
    }).toThrowWithMessage(
        TypeError,
        "Expected target argument of Proxy constructor to be object, got undefined"
    );

    expect(() => {
        new Proxy({});
    }).toThrowWithMessage(
        TypeError,
        "Expected handler argument of Proxy constructor to be object, got undefined"
    );
});

test("constructor requires objects", () => {
    expect(() => {
        new Proxy(1, {});
    }).toThrowWithMessage(
        TypeError,
        "Expected target argument of Proxy constructor to be object, got 1"
    );

    expect(() => {
        new Proxy({}, 1);
    }).toThrowWithMessage(
        TypeError,
        "Expected handler argument of Proxy constructor to be object, got 1"
    );
});

test("constructor must be invoked with 'new'", () => {
    expect(() => {
        Proxy({}, {});
    }).toThrowWithMessage(TypeError, "Proxy constructor must be called with 'new'");
});
