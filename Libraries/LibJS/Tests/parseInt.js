test("basic parseInt() functionality", () => {
    expect(parseInt("0")).toBe(0);
    expect(parseInt("100")).toBe(100);
    expect(parseInt("1000", 16)).toBe(4096);
    expect(parseInt("0xF", 16)).toBe(15);
    expect(parseInt("F", 16)).toBe(15);
    expect(parseInt("17", 8)).toBe(15);
    expect(parseInt(021, 8)).toBe(15);
    expect(parseInt("015", 10)).toBe(15);
    expect(parseInt(15.99, 10)).toBe(15);
    expect(parseInt("15,123", 10)).toBe(15);
    expect(parseInt("FXX123", 16)).toBe(15);
    expect(parseInt("1111", 2)).toBe(15);
    expect(parseInt("15 * 3", 10)).toBe(15);
    expect(parseInt("15e2", 10)).toBe(15);
    expect(parseInt("15px", 10)).toBe(15);
    expect(parseInt("12", 13)).toBe(15);
    expect(parseInt("Hello", 8)).toBeNaN();
    expect(parseInt("546", 2)).toBeNaN();
    expect(parseInt("-F", 16)).toBe(-15);
    expect(parseInt("-0F", 16)).toBe(-15);
    expect(parseInt("-0XF", 16)).toBe(-15);
    expect(parseInt(-15.1, 10)).toBe(-15);
    expect(parseInt("-17", 8)).toBe(-15);
    expect(parseInt("-15", 10)).toBe(-15);
    expect(parseInt("-1111", 2)).toBe(-15);
    expect(parseInt("-15e1", 10)).toBe(-15);
    expect(parseInt("-12", 13)).toBe(-15);
    expect(parseInt(4.7, 10)).toBe(4);
    expect(parseInt("0e0", 16)).toBe(224);
    expect(parseInt("123_456")).toBe(123);
    expect(parseInt("UVWXYZ", 36)).toBe(1867590395);
    expect(parseInt(4.7 * 1e22, 10)).toBe(4);
    expect(parseInt(0.00000000000434, 10)).toBe(4);
    expect(parseInt(0.0000001, 11)).toBe(1);
    expect(parseInt(0.000000124, 10)).toBe(1);
    expect(parseInt(1e-7, 10)).toBe(1);
    expect(parseInt(1000000000000100000000, 10)).toBe(1);
    expect(parseInt(123000000000010000000000, 10)).toBe(1);
    expect(parseInt(1e21, 10)).toBe(1);
    // FIXME: expect(parseInt('900719925474099267n')).toBe(900719925474099300)
});

test("parseInt() radix is coerced to a number", () => {
    const obj = {
        valueOf() {
            return 8;
        },
    };
    expect(parseInt("11", obj)).toBe(9);
    obj.valueOf = function () {
        return 1;
    };
    expect(parseInt("11", obj)).toBeNaN();
    obj.valueOf = function () {
        return Infinity;
    };
    expect(parseInt("11", obj)).toBe(11);
});
