test("normal methods named get and set", () => {
    let o = {
        get() {
            return 5;
        },
        set() {
            return 10;
        },
    };
    expect(o.get()).toBe(5);
    expect(o.set()).toBe(10);
});

test("basic get and set", () => {
    let o = {
        get x() {
            return 5;
        },
        set x(_) {},
    };
    expect(o.x).toBe(5);
    o.x = 10;
    expect(o.x).toBe(5);
});

test("get and set with 'this'", () => {
    let o = {
        get x() {
            return this._x + 1;
        },
        set x(value) {
            this._x = value + 1;
        },
    };

    expect(o.x).toBeNaN();
    o.x = 10;
    expect(o.x).toBe(12);
    o.x = 20;
    expect(o.x).toBe(22);
});

test("multiple getters", () => {
    let o = {
        get x() {
            return 5;
        },
        get x() {
            return 10;
        },
    };
    expect(o.x).toBe(10);
});

test("setter return value", () => {
    o = {
        set x(value) {
            return 10;
        },
    };
    expect((o.x = 20)).toBe(20);
});
