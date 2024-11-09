test("basic numeric shifting", () => {
    expect(0 << 0).toBe(0);
    expect(0 << 1).toBe(0);
    expect(0 << 2).toBe(0);
    expect(0 << 3).toBe(0);
    expect(0 << 4).toBe(0);
    expect(0 << 5).toBe(0);

    expect(1 << 0).toBe(1);
    expect(1 << 1).toBe(2);
    expect(1 << 2).toBe(4);
    expect(1 << 3).toBe(8);
    expect(1 << 4).toBe(16);
    expect(1 << 5).toBe(32);

    expect(2 << 0).toBe(2);
    expect(2 << 1).toBe(4);
    expect(2 << 2).toBe(8);
    expect(2 << 3).toBe(16);
    expect(2 << 4).toBe(32);
    expect(2 << 5).toBe(64);

    expect(3 << 0).toBe(3);
    expect(3 << 1).toBe(6);
    expect(3 << 2).toBe(12);
    expect(3 << 3).toBe(24);
    expect(3 << 4).toBe(48);
    expect(3 << 5).toBe(96);

    expect(4 << 0).toBe(4);
    expect(4 << 1).toBe(8);
    expect(4 << 2).toBe(16);
    expect(4 << 3).toBe(32);
    expect(4 << 4).toBe(64);
    expect(4 << 5).toBe(128);

    expect(5 << 0).toBe(5);
    expect(5 << 1).toBe(10);
    expect(5 << 2).toBe(20);
    expect(5 << 3).toBe(40);
    expect(5 << 4).toBe(80);
    expect(5 << 5).toBe(160);

    expect(0xffffffff << 0).toBe(-1);
    expect(0xffffffff << 16).toBe(-65536);
    expect(0xffff0000 << 16).toBe(0);
    expect(0xffff0000 << 15).toBe(-2147483648);
});

test("shifting with non-numeric values", () => {
    let x = 3;
    let y = 7;

    expect("42" << 6).toBe(2688);
    expect(x << y).toBe(384);
    expect(x << [[[[12]]]]).toBe(12288);
    expect(undefined << y).toBe(0);
    expect("a" << "b").toBe(0);
    expect(null << null).toBe(0);
    expect(undefined << undefined).toBe(0);
    expect(NaN << NaN).toBe(0);
    expect(NaN << 6).toBe(0);
    expect(Infinity << Infinity).toBe(0);
    expect(-Infinity << Infinity).toBe(0);
});

describe("logical left shift on big ints", () => {
    expect(3n << -1n).toBe(1n);
    expect(3n << -2n).toBe(0n);
    expect(-3n << -1n).toBe(-2n);
    expect(-3n << -2n).toBe(-1n);
    expect(-3n << -128n).toBe(-1n);
    expect(3n << 6n).toBe(192n);
    expect(3n << 0n).toBe(3n);
});
