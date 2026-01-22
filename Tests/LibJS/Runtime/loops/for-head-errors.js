test("using undefined variable in initializer", () => {
    expect(() => {
        for (let i = foo; i < 100; ++i) {}
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");
});

test("using undefined variable in condition", () => {
    expect(() => {
        for (let i = 0; i < foo; ++i) {}
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");
});

test("using undefined variable in updater", () => {
    let loopCount = 0;

    expect(() => {
        for (let i = 0; i < 100; ++foo) {
            loopCount++;
        }
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

    expect(loopCount).toBe(1);
});
