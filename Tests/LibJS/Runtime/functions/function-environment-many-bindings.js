test("a hot function keeps more bindings than fit in its environment's cell", () => {
    function many(value) {
        var a0 = value + 0,
            a1 = value + 1,
            a2 = value + 2,
            a3 = value + 3,
            a4 = value + 4,
            a5 = value + 5;
        var a6 = value + 6,
            a7 = value + 7,
            a8 = value + 8,
            a9 = value + 9,
            a10 = value + 10,
            a11 = value + 11;
        var a12 = value + 12,
            a13 = value + 13,
            a14 = value + 14,
            a15 = value + 15,
            a16 = value + 16;
        var a17 = value + 17,
            a18 = value + 18,
            a19 = value + 19;
        return eval("a0 + a10 + a19");
    }

    for (let i = 0; i < 5000; ++i) expect(many(i)).toBe(3 * i + 29);
});

test("a binding created by a direct eval joins the many others", () => {
    function many() {
        var a0 = 0,
            a1 = 1,
            a2 = 2,
            a3 = 3,
            a4 = 4,
            a5 = 5,
            a6 = 6,
            a7 = 7,
            a8 = 8,
            a9 = 9;
        var a10 = 10,
            a11 = 11,
            a12 = 12,
            a13 = 13,
            a14 = 14,
            a15 = 15,
            a16 = 16,
            a17 = 17;
        eval("var added = a17 + 1;");
        return added;
    }

    for (let i = 0; i < 5000; ++i) expect(many()).toBe(18);
});
