describe("Issue #3382", () => {
    test("Creating an array with simple storage (<= 200 initial elements)", () => {
        var a = Array(200);
        expect(a).toHaveLength(200);
        expect(a.push("foo")).toBe(201);
        expect(a).toHaveLength(201);
    });

    test("Creating an array with generic storage (> 200 initial elements)", () => {
        var a = Array(201);
        expect(a).toHaveLength(201);
        expect(a.push("foo")).toBe(202);
        expect(a).toHaveLength(202);
    });
});
