describe("a let initializer cannot clear its own binding's TDZ sentinel", () => {
    test("object literal", () => {
        expect(() => {
            let value = { property: (value = "assigned too early") };
        }).toThrowWithMessage(ReferenceError, "is not initialized");
    });

    test("array literal", () => {
        expect(() => {
            let value = [(value = "assigned too early")];
        }).toThrowWithMessage(ReferenceError, "is not initialized");
    });

    test("template literal", () => {
        expect(() => {
            let value = `${(value = "assigned too early")}`;
        }).toThrowWithMessage(ReferenceError, "is not initialized");
    });

    test("shorthand property", () => {
        expect(() => {
            let value = { value };
        }).toThrowWithMessage(ReferenceError, "is not initialized");
    });

    test("const declaration", () => {
        expect(() => {
            const value = { property: (value = "assigned too early") };
        }).toThrowWithMessage(ReferenceError, "is not initialized");
    });
});

test("a let initializer that cannot reach its binding still works", () => {
    let object = { a: 1, b: 2 };
    expect(object.a).toBe(1);
    expect(object.b).toBe(2);

    let array = [1, 2, 3];
    expect(array.length).toBe(3);
});
