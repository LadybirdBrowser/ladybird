const cases = [
    ["1;", 1],
    ["1; var a = 2", 1],
    ["1; if (true) {}", undefined],
    ["1; if (false) {}", undefined],
    ["1; if (false) {} else { 3 }", 3],
    ["1; while (false);", undefined],
    ["1; do { 2 } while (false)", 2],
    ["1; do { 2; if (1) { break } } while (false)", undefined],
    ["1; do { 2; { break } } while (false)", 2],
    ["1; for (var i = 0; i < 2; i++) { i }", 1],
    ["1; for (var k of []) { k }", undefined],
    ["1; l: { 2; { break l; 3 } }", 2],
    ["1; switch (1) {}", undefined],
    ["1; switch (2) { default: 2; case 1: 3 }", 3],
    ["1; try { 2 } catch (e) {}", 2],
    ["1; try { throw 2 } catch (e) {}", undefined],
    ["1; try { 2 } finally { 3 }", 2],
    ["do { 1; try { 2; break } finally { 3 } } while (0)", 2],
    ["do { 1; try { 2 } finally { 3; break } } while (0)", 3],
    ["do { 1; try { 2 } finally { break } } while (0)", undefined],
    ["1; with ({}) {}", undefined],
    ["1; with ({}) { 2 }", 2],
    ["1; { let a = 1 }", 1],
    ["1; class C {}", 1],
    ["{ let a = 1; a; let b = (a = 9); }", 1],
    ["{ let a = 1; a++; }", 1],
    ["{ let a = 5; a--; }", 5],
    ["{ let a = '5'; a++ }", 5],
    ["{ let a = 1n; a-- }", 1n],
    ["{ let i = 0; while (i < 3) i++; }", 2],
    ["for (let i = 0; i < 2; i++) { i++; }", 0],
    ["try { throw 1 } catch (e) { e++ }", 1],
    ["{ let a = 1; ++a; }", 2],
];

test("the value of statements in eval code", () => {
    for (const [source, value] of cases) {
        expect(eval(source)).toBe(value);
        expect((0, eval)(source)).toBe(value);
    }
});
