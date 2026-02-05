test("variable named 'eval' pointing to another function calls that function", function () {
    var testValue = "inner";
    // This breaks prettier as it considers this to be a parse error
    // before even trying to do any linting
    var eval = () => {
        return "wat";
    };
    expect(eval("testValue")).toEqual("wat");
});

test("variable named 'eval' pointing to real eval works as a direct eval", function () {
    var testValue = "inner";
    var eval = globalThis.eval;
    expect(eval("testValue")).toEqual("inner");
});

test("variable named 'eval' pointing to a non-function raises a TypeError", function () {
    var eval = "borked";
    expect(() => eval("something").toThrowWithMessage(TypeError, "borked is not a function"));
});
