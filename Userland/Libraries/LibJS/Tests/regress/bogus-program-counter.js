test("Don't crash when throwing exception inside a callee smaller than the caller", () => {
    function make() {
        let o = {};
        Object.defineProperty(o, "go", {
            get: function () {
                return doesNotExist;
            },
        });
        return o;
    }

    // Some nonsense to make sure this function has a longer bytecode than the throwing getter.
    function x() {
        return 3;
    }
    function b() {}
    b(x() + x() + x());

    expect(() => make().go()).toThrowWithMessage(ReferenceError, "'doesNotExist' is not defined");
});
