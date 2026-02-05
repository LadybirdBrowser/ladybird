test("basic numeric or", () => {
    expect(0 | 0).toBe(0);
    expect(0 | 1).toBe(1);
    expect(0 | 2).toBe(2);
    expect(0 | 3).toBe(3);
    expect(0 | 4).toBe(4);
    expect(0 | 5).toBe(5);

    expect(1 | 0).toBe(1);
    expect(1 | 1).toBe(1);
    expect(1 | 2).toBe(3);
    expect(1 | 3).toBe(3);
    expect(1 | 4).toBe(5);
    expect(1 | 5).toBe(5);

    expect(2 | 0).toBe(2);
    expect(2 | 1).toBe(3);
    expect(2 | 2).toBe(2);
    expect(2 | 3).toBe(3);
    expect(2 | 4).toBe(6);
    expect(2 | 5).toBe(7);

    expect(3 | 0).toBe(3);
    expect(3 | 1).toBe(3);
    expect(3 | 2).toBe(3);
    expect(3 | 3).toBe(3);
    expect(3 | 4).toBe(7);
    expect(3 | 5).toBe(7);

    expect(4 | 0).toBe(4);
    expect(4 | 1).toBe(5);
    expect(4 | 2).toBe(6);
    expect(4 | 3).toBe(7);
    expect(4 | 4).toBe(4);
    expect(4 | 5).toBe(5);

    expect(5 | 0).toBe(5);
    expect(5 | 1).toBe(5);
    expect(5 | 2).toBe(7);
    expect(5 | 3).toBe(7);
    expect(5 | 4).toBe(5);
    expect(5 | 5).toBe(5);

    expect(0xffffffff | 0).toBe(-1);
    expect(0xffffffff | 0xffffffff).toBe(-1);
    expect(2147483648 | 0).toBe(-2147483648);
});

test("or with non-numeric values", () => {
    let x = 3;
    let y = 7;

    expect("42" | 6).toBe(46);
    expect(x | y).toBe(7);
    expect(x | [[[[12]]]]).toBe(15);
    expect(undefined | y).toBe(7);
    expect("a" | "b").toBe(0);
    expect(null | null).toBe(0);
    expect(undefined | undefined).toBe(0);
    expect(NaN | NaN).toBe(0);
    expect(NaN | 6).toBe(6);
    expect(Infinity | Infinity).toBe(0);
    expect(-Infinity | Infinity).toBe(0);
});
