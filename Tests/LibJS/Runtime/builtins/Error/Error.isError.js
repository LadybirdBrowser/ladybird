test("Error.isError length is 1", () => {
    expect(Error.isError).toHaveLength(1);
});

test("Error.isError arguments that evaluate to false", () => {
    expect(Error.isError()).toBeFalse();
    expect(Error.isError("1")).toBeFalse();
    expect(Error.isError("foo")).toBeFalse();
    expect(Error.isError(1)).toBeFalse();
    expect(Error.isError(1, 2, 3)).toBeFalse();
    expect(Error.isError(undefined)).toBeFalse();
    expect(Error.isError(null)).toBeFalse();
    expect(Error.isError(Infinity)).toBeFalse();
    expect(Error.isError({})).toBeFalse();
});

test("Error.isError arguments that evaluate to true", () => {
    expect(Error.isError(new Error())).toBeTrue();
    expect(Error.isError(new EvalError())).toBeTrue();
    expect(Error.isError(new RangeError())).toBeTrue();
    expect(Error.isError(new ReferenceError())).toBeTrue();
    expect(Error.isError(new SyntaxError())).toBeTrue();
    expect(Error.isError(new TypeError())).toBeTrue();
    expect(Error.isError(new URIError())).toBeTrue();
    expect(Error.isError(new SuppressedError())).toBeTrue();

    class MySuppressedError extends SuppressedError {}
    expect(Error.isError(new MySuppressedError())).toBeTrue();

    class MyTypeError extends TypeError {}
    expect(Error.isError(new MyTypeError())).toBeTrue();
});
