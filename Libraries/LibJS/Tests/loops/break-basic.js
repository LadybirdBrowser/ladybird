test("Toplevel break inside loop", () => {
    var j = 0;
    for (var i = 0; i < 9; ++i) {
        break;
        ++j;
    }
    expect(j).toBe(0);
});

test("break inside sub-blocks", () => {
    var j = 0;
    for (var i = 0; i < 9; ++i) {
        if (j == 4) {
            break;
        }
        ++j;
    }
    expect(j).toBe(4);
});

test("break inside curly sub-blocks", () => {
    var j = 0;
    for (var i = 0; i < 9; ++i) {
        if (j == 4) {
            break;
        }
        ++j;
    }
    expect(j).toBe(4);
});
