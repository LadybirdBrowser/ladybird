test("Assignment should always evaluate LHS first", () => {
    function go(a) {
        let i = 0;
        a[i] = a[++i];
    }

    let a = [1, 2, 3];
    go(a);
    expect(a).toEqual([2, 2, 3]);
});

test("Binary assignment should always evaluate LHS first", () => {
    function go(a) {
        let i = 0;
        a[i] |= a[++i];
    }

    let a = [1, 2];
    go(a);
    expect(a).toEqual([3, 2]);
});
