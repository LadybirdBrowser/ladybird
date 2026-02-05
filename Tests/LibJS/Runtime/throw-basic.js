test("throw literal", () => {
    try {
        throw 1;
        expect().fail();
    } catch (e) {
        if (e.name === "ExpectationError") throw e;
        expect(e).toBe(1);
    }
});

test("throw array", () => {
    try {
        throw [99];
        expect().fail();
    } catch (e) {
        if (e.name === "ExpectationError") throw e;
        expect(e).toEqual([99]);
    }
});

test("call function that throws", () => {
    function foo() {
        throw "hello";
        expect().fail();
    }

    try {
        foo();
        expect().fail();
    } catch (e) {
        if (e.name === "ExpectationError") throw e;
        expect(e).toBe("hello");
    }
});
