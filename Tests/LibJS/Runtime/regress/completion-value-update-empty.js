describe("UpdateEmpty completion value semantics for loops and switch", () => {
    test("while: break with no value keeps undefined", () => {
        expect(eval("1; while (true) { break; }")).toBeUndefined();
    });

    test("while: break preserves last value", () => {
        expect(eval("2; while (true) { 3; break; }")).toBe(3);
    });

    test("do-while: break with no value keeps undefined", () => {
        expect(eval("1; do { break; } while (false)")).toBeUndefined();
    });

    test("do-while: break preserves last value", () => {
        expect(eval("2; do { 3; break; } while (false)")).toBe(3);
    });

    test("for: break with no value keeps undefined", () => {
        expect(eval("1; for (;;) { break; }")).toBeUndefined();
    });

    test("for: break preserves last value", () => {
        expect(eval("2; for (;;) { 3; break; }")).toBe(3);
    });

    test("for: empty body returns undefined", () => {
        expect(eval("2; for (var i = 0; i < 3; i++) { }")).toBeUndefined();
    });

    test("switch: break with no value keeps undefined", () => {
        expect(eval('1; switch ("a") { case "a": break; }')).toBeUndefined();
    });

    test("switch: break preserves last value", () => {
        expect(eval('2; switch ("a") { case "a": { 3; break; } }')).toBe(3);
    });

    test("nested: continue outer with value", () => {
        expect(eval("5; outer: do { while (true) { 6; continue outer; } } while (false)")).toBe(6);
    });

    test("nested: continue outer with no value keeps undefined", () => {
        expect(eval("4; outer: do { while (true) { continue outer; } } while (false)")).toBeUndefined();
    });

    test("nested: break outer with value", () => {
        expect(eval("1; outer: while (true) { while (true) { 7; break outer; } }")).toBe(7);
    });

    test("for-in: break preserves last value", () => {
        expect(eval("1; for (var x in {a: 1}) { 8; break; }")).toBe(8);
    });

    test("for-of: break preserves last value", () => {
        expect(eval("1; for (var x of [1]) { 9; break; }")).toBe(9);
    });
});
