test("reassigning undefined should have no effect", () => {
    function attemptToChangeGlobalUndefined() {
        undefined = 42;
        eval("undefined = 42");
    }

    function modifyUndefinedInFunctionScope() {
        var undefined = 42;
        expect(undefined).toBe(42);
    }

    attemptToChangeGlobalUndefined();
    modifyUndefinedInFunctionScope();
    expect(undefined).toBeUndefined();
});
