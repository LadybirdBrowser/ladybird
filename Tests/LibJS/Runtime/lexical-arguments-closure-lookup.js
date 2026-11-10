describe("closure lookup from a function that creates no arguments object", () => {
    function enclose(makeFunction) {
        const decoy = "decoy";
        const inner = makeFunction("captured");
        return [inner, () => decoy];
    }

    test("let declaration after a read of arguments", () => {
        const [read] = enclose(
            value =>
                function (index) {
                    if (index === 0) arguments;
                    let arguments = 1;
                    return value;
                }
        );
        expect(read(1)).toBe("captured");
    });

    test("const declaration after typeof arguments", () => {
        const [read] = enclose(
            value =>
                function (index) {
                    if (index === 0) typeof arguments;
                    const arguments = 1;
                    return value;
                }
        );
        expect(read(1)).toBe("captured");
    });

    test("assignment after a read of arguments", () => {
        let readValue;
        const [write, readDecoy] = enclose(value => {
            readValue = () => value;
            return function (index) {
                if (index === 0) arguments;
                let arguments = 1;
                value = "written";
            };
        });
        write(1);
        expect(readValue()).toBe("written");
        expect(readDecoy()).toBe("decoy");
    });

    test("parameter named arguments and an arrow function that declares arguments", () => {
        const [read] = enclose(
            value =>
                function (arguments) {
                    (() => {
                        if (false) arguments;
                        let arguments = 1;
                    })();
                    return value;
                }
        );
        expect(read(1)).toBe("captured");
    });
});
