// A sloppy-mode function whose parameters aren't simple gets an unmapped arguments object, which needs no environment.
// Reading a variable of an enclosing function from such a function has to find that function's binding.
describe("closure lookup from a function with an unmapped arguments object", () => {
    function enclose(makeReader) {
        const decoy = "decoy";
        const read = makeReader("captured");
        return [read, () => decoy];
    }

    test("default parameter", () => {
        const [read] = enclose(
            value =>
                function (index, flag = false) {
                    if (arguments.length) return value;
                }
        );
        expect(read(0)).toBe("captured");
    });

    test("rest parameter", () => {
        const [read] = enclose(
            value =>
                function (...rest) {
                    arguments;
                    return value;
                }
        );
        expect(read(0)).toBe("captured");
    });

    test("destructuring parameter", () => {
        const [read] = enclose(
            value =>
                function ({ a } = {}) {
                    arguments;
                    return value;
                }
        );
        expect(read()).toBe("captured");
    });

    test("default parameter reading arguments", () => {
        const [read] = enclose(
            value =>
                function (index, count = arguments.length) {
                    return value;
                }
        );
        expect(read(0)).toBe("captured");
    });

    test("method with a default parameter", () => {
        const [read] = enclose(
            value =>
                ({
                    method(index, flag = false) {
                        arguments;
                        return value;
                    },
                }).method
        );
        expect(read(0)).toBe("captured");
    });
});
