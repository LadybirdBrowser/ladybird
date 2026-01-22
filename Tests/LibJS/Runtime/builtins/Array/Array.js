test("constructor properties", () => {
    expect(Array).toHaveLength(1);
    expect(Array.name).toBe("Array");
    expect(Array.prototype.length).toBe(0);
});

describe("errors", () => {
    test("invalid array length", () => {
        [-1, -100, -0.1, 0.1, 1.23, Infinity, -Infinity, NaN].forEach(value => {
            expect(() => {
                new Array(value);
            }).toThrowWithMessage(RangeError, "Invalid array length");
        });
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof Array()).toBe("object");
        expect(typeof new Array()).toBe("object");
    });

    test("constructor with single numeric argument", () => {
        var a = new Array(5);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(5);
    });

    test("constructor with single non-numeric argument", () => {
        var a = new Array("5");
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0]).toBe("5");
    });

    test("constructor with multiple numeric arguments", () => {
        var a = new Array(1, 2, 3);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
    });

    test("constructor with single array argument", () => {
        var a = new Array([1, 2, 3]);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0][0]).toBe(1);
        expect(a[0][1]).toBe(2);
        expect(a[0][2]).toBe(3);
    });
});
