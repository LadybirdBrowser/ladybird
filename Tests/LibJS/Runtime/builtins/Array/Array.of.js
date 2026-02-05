test("length is 0", () => {
    expect(Array.of).toHaveLength(0);
});

describe("normal behavior", () => {
    test("single numeric argument", () => {
        var a = Array.of(5);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0]).toBe(5);
    });

    test("single non-numeric argument", () => {
        var a = Array.of("5");
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0]).toBe("5");
    });

    test("single infinite numeric argument", () => {
        var a = Array.of(Infinity);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0]).toBe(Infinity);
    });

    test("multiple numeric arguments", () => {
        var a = Array.of(1, 2, 3);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
    });

    test("single array argument", () => {
        var a = Array.of([1, 2, 3]);
        expect(a instanceof Array).toBeTrue();
        expect(a).toHaveLength(1);
        expect(a[0][0]).toBe(1);
        expect(a[0][1]).toBe(2);
        expect(a[0][2]).toBe(3);
    });

    test("getter property is included in returned array", () => {
        var t = [1, 2, 3];
        Object.defineProperty(t, 3, {
            get() {
                return 4;
            },
        });
        var a = Array.of(...t);
        expect(a).toHaveLength(4);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
        expect(a[3]).toBe(4);
    });
});
