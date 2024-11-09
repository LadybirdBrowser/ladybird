describe("normal behavior", () => {
    test("regular object indexing", () => {
        const o = {};
        const p = { 0: "foo" };
        Object.setPrototypeOf(o, p);
        expect(o[0]).toBe("foo");
    });

    test("array object indexing", () => {
        const o = [];
        const p = ["foo"];
        Object.setPrototypeOf(o, p);
        expect(o[0]).toBe("foo");
    });

    test("array object hole indexing", () => {
        const o = [,];
        const p = ["foo"];
        Object.setPrototypeOf(o, p);
        expect(o[0]).toBe("foo");
    });

    test("string object indexing", () => {
        const o = new String("");
        const p = new String("a");
        Object.setPrototypeOf(o, p);
        expect(o[0]).toBe("a");
    });
});
