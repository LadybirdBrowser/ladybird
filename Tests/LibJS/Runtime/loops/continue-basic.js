test("basic functionality", () => {
    var j = 0;
    for (var i = 0; i < 9; ++i) {
        if (i == 3) {
            continue;
        }
        ++j;
    }
    expect(j).toBe(8);
});
