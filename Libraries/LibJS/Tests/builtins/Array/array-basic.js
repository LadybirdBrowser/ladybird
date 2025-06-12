test("basic functionality", () => {
    var a = [1, 2, 3];

    expect(typeof a).toBe("object");
    expect(a).toHaveLength(3);
    expect(a[0]).toBe(1);
    expect(a[1]).toBe(2);
    expect(a[2]).toBe(3);

    a[1] = 5;
    expect(a[1]).toBe(5);
    expect(a).toHaveLength(3);

    a.push(7);
    expect(a[3]).toBe(7);
    expect(a).toHaveLength(4);

    a = [,];
    expect(a).toHaveLength(1);
    expect(a.toString()).toBe("");
    expect(a[0]).toBeUndefined();

    a = [, , , ,];
    expect(a).toHaveLength(4);
    expect(a.toString()).toBe(",,,");
    expect(a[0]).toBeUndefined();
    expect(a[1]).toBeUndefined();
    expect(a[2]).toBeUndefined();
    expect(a[3]).toBeUndefined();

    a = [1, , 2, , , 3];
    expect(a).toHaveLength(6);
    expect(a.toString()).toBe("1,,2,,,3");
    expect(a[0]).toBe(1);
    expect(a[1]).toBeUndefined();
    expect(a[2]).toBe(2);
    expect(a[3]).toBeUndefined();
    expect(a[4]).toBeUndefined();
    expect(a[5]).toBe(3);

    a = [1, , 2, , , 3];
    Object.defineProperty(a, 1, {
        get() {
            return this.getterSetterValue;
        },
        set(value) {
            this.getterSetterValue = value;
        },
    });
    expect(a).toHaveLength(6);
    expect(a.toString()).toBe("1,,2,,,3");
    expect(a.getterSetterValue).toBeUndefined();
    a[1] = 20;
    expect(a).toHaveLength(6);
    expect(a.toString()).toBe("1,20,2,,,3");
    expect(a.getterSetterValue).toBe(20);
});

test("assigning array expression with destination referenced in array expression", () => {
    function go(i) {
        var i = [i];
        return i;
    }
    expect(go("foo")).toEqual(["foo"]);
});
