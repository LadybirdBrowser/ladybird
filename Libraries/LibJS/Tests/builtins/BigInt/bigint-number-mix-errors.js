const doTest = (operatorName, executeOperation) => {
    [1, null, undefined].forEach(value => {
        const messageSuffix = operatorName === "unsigned right-shift" ? "" : " and other type";

        expect(() => {
            executeOperation(1n, value);
        }).toThrowWithMessage(
            TypeError,
            `Cannot use ${operatorName} operator with BigInt${messageSuffix}`
        );
    });
};

[
    ["addition", (a, b) => a + b],
    ["subtraction", (a, b) => a - b],
    ["multiplication", (a, b) => a * b],
    ["division", (a, b) => a / b],
    ["modulo", (a, b) => a % b],
    ["exponentiation", (a, b) => a ** b],
    ["bitwise OR", (a, b) => a | b],
    ["bitwise AND", (a, b) => a & b],
    ["bitwise XOR", (a, b) => a ^ b],
    ["left-shift", (a, b) => a << b],
    ["right-shift", (a, b) => a >> b],
    ["unsigned right-shift", (a, b) => a >>> b],
].forEach(testCase => {
    test(`using ${testCase[0]} operator with BigInt and other type`, () => {
        doTest(testCase[0], testCase[1]);
    });
});
