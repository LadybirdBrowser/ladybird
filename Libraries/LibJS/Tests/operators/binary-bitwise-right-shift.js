test("basic numeric shifting", () => {
    expect(0 >> 0).toBe(0);
    expect(0 >> 1).toBe(0);
    expect(0 >> 2).toBe(0);
    expect(0 >> 3).toBe(0);
    expect(0 >> 4).toBe(0);
    expect(0 >> 5).toBe(0);

    expect(1 >> 0).toBe(1);
    expect(1 >> 1).toBe(0);
    expect(1 >> 2).toBe(0);
    expect(1 >> 3).toBe(0);
    expect(1 >> 4).toBe(0);
    expect(1 >> 5).toBe(0);

    expect(5 >> 0).toBe(5);
    expect(5 >> 1).toBe(2);
    expect(5 >> 2).toBe(1);
    expect(5 >> 3).toBe(0);
    expect(5 >> 4).toBe(0);
    expect(5 >> 5).toBe(0);

    expect(42 >> 0).toBe(42);
    expect(42 >> 1).toBe(21);
    expect(42 >> 2).toBe(10);
    expect(42 >> 3).toBe(5);
    expect(42 >> 4).toBe(2);
    expect(42 >> 5).toBe(1);

    expect(0xffffffff >> 0).toBe(-1);
    expect(0xffffffff >> 16).toBe(-1);
    expect((0xf0000000 * 2) >> 16).toBe(-8192);
});

test("numeric shifting with negative lhs values", () => {
    expect(-1 >> 0).toBe(-1);
    expect(-1 >> 1).toBe(-1);
    expect(-1 >> 2).toBe(-1);
    expect(-1 >> 3).toBe(-1);
    expect(-1 >> 4).toBe(-1);
    expect(-1 >> 5).toBe(-1);

    expect(-5 >> 0).toBe(-5);
    expect(-5 >> 1).toBe(-3);
    expect(-5 >> 2).toBe(-2);
    expect(-5 >> 3).toBe(-1);
    expect(-5 >> 4).toBe(-1);
    expect(-5 >> 5).toBe(-1);
});

test("shifting with non-numeric values", () => {
    let x = 67;
    let y = 4;

    expect("42" >> 3).toBe(5);
    expect(x >> y).toBe(4);
    expect(x >> [[[[5]]]]).toBe(2);
    expect(undefined >> y).toBe(0);
    expect("a" >> "b").toBe(0);
    expect(null >> null).toBe(0);
    expect(undefined >> undefined).toBe(0);
    expect(NaN >> NaN).toBe(0);
    expect(6 >> NaN).toBe(6);
    expect(Infinity >> Infinity).toBe(0);
    expect(-Infinity >> Infinity).toBe(0);
});

describe("logical right shift on big ints", () => {
    expect(3n >> 1n).toBe(1n);
    expect(3n >> 2n).toBe(0n);
    expect(-3n >> 1n).toBe(-2n);
    expect(-3n >> 2n).toBe(-1n);
    expect(-3n >> 128n).toBe(-1n);
    expect(-3n >> -6n).toBe(-192n);
    expect(-3n >> 0n).toBe(-3n);
});
