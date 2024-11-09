test("basic numeric shifting", () => {
    expect(0 >>> 0).toBe(0);
    expect(0 >>> 1).toBe(0);
    expect(0 >>> 2).toBe(0);
    expect(0 >>> 3).toBe(0);
    expect(0 >>> 4).toBe(0);
    expect(0 >>> 5).toBe(0);

    expect(1 >>> 0).toBe(1);
    expect(1 >>> 1).toBe(0);
    expect(1 >>> 2).toBe(0);
    expect(1 >>> 3).toBe(0);
    expect(1 >>> 4).toBe(0);
    expect(1 >>> 5).toBe(0);

    expect(5 >>> 0).toBe(5);
    expect(5 >>> 1).toBe(2);
    expect(5 >>> 2).toBe(1);
    expect(5 >>> 3).toBe(0);
    expect(5 >>> 4).toBe(0);
    expect(5 >>> 5).toBe(0);

    expect(42 >>> 0).toBe(42);
    expect(42 >>> 1).toBe(21);
    expect(42 >>> 2).toBe(10);
    expect(42 >>> 3).toBe(5);
    expect(42 >>> 4).toBe(2);
    expect(42 >>> 5).toBe(1);

    expect(0xffffffff >>> 0).toBe(4294967295);
    expect(0xffffffff >>> 16).toBe(65535);
    expect((0xf0000000 * 2) >>> 16).toBe(57344);
});

test("numeric shifting with negative lhs values", () => {
    expect(-1 >>> 0).toBe(4294967295);
    expect(-1 >>> 1).toBe(2147483647);
    expect(-1 >>> 2).toBe(1073741823);
    expect(-1 >>> 3).toBe(536870911);
    expect(-1 >>> 4).toBe(268435455);
    expect(-1 >>> 5).toBe(134217727);

    expect(-5 >>> 0).toBe(4294967291);
    expect(-5 >>> 1).toBe(2147483645);
    expect(-5 >>> 2).toBe(1073741822);
    expect(-5 >>> 3).toBe(536870911);
    expect(-5 >>> 4).toBe(268435455);
    expect(-5 >>> 5).toBe(134217727);
});

test("shifting with non-numeric values", () => {
    let x = -67;
    let y = 4;

    expect("-42" >>> 3).toBe(536870906);
    expect(x >>> y).toBe(268435451);
    expect(x >>> [[[[5]]]]).toBe(134217725);
    expect(undefined >>> y).toBe(0);
    expect("a" >>> "b").toBe(0);
    expect(null >>> null).toBe(0);
    expect(undefined >>> undefined).toBe(0);
    expect(NaN >>> NaN).toBe(0);
    expect(6 >>> NaN).toBe(6);
    expect(Infinity >>> Infinity).toBe(0);
    expect(-Infinity >>> Infinity).toBe(0);
});
