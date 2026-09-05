test("Inline cache invalidated by deleting property from unique shape", () => {
    // Create an object with an unique shape by adding a huge amount of properties.
    let o = {};
    for (let x = 0; x < 1000; ++x) {
        o["prop" + x] = x;
    }

    function ic(o) {
        return o.prop2;
    }

    let first = ic(o);
    delete o.prop2;
    let second = ic(o);

    expect(first).toBe(2);
    expect(second).toBeUndefined();
});

function createObjectWithUniqueShape() {
    let o = {};
    for (let x = 0; x < 1000; ++x) {
        o["prop" + x] = x;
    }
    for (let x = 0; x < 1000; ++x) {
        delete o["prop" + x];
    }
    return o;
}

test("Getter that deletes itself from a unique shape", () => {
    let o = createObjectWithUniqueShape();
    Object.defineProperty(o, "accessor", {
        get() {
            delete o.accessor;
            return "getter";
        },
        configurable: true,
    });
    o.neighbor = "neighbor";

    function ic(o) {
        return o.accessor;
    }

    expect(ic(o)).toBe("getter");
    expect(ic(o)).toBeUndefined();
    expect(o.neighbor).toBe("neighbor");
});

test("Setter that deletes itself from a unique shape", () => {
    let o = createObjectWithUniqueShape();
    Object.defineProperty(o, "accessor", {
        set(value) {
            delete o.accessor;
        },
        configurable: true,
    });
    o.neighbor = "neighbor";

    function ic(o, value) {
        o.accessor = value;
    }

    ic(o, 1);
    ic(o, 2);
    expect(o.neighbor).toBe("neighbor");
    expect(o.accessor).toBe(2);
});

test("Global getter that deletes itself", () => {
    Object.defineProperty(globalThis, "selfDeletingGlobalGetter", {
        get() {
            delete globalThis.selfDeletingGlobalGetter;
            return "getter";
        },
        configurable: true,
    });
    globalThis.globalNeighbor = "neighbor";

    function ic() {
        return selfDeletingGlobalGetter;
    }

    expect(ic()).toBe("getter");
    expect(ic).toThrowWithMessage(ReferenceError, "'selfDeletingGlobalGetter' is not defined");
    expect(globalThis.globalNeighbor).toBe("neighbor");
});

test("Global setter that deletes itself", () => {
    Object.defineProperty(globalThis, "selfDeletingGlobalSetter", {
        set(value) {
            delete globalThis.selfDeletingGlobalSetter;
        },
        configurable: true,
    });
    globalThis.globalSetterNeighbor = "neighbor";

    function ic(value) {
        selfDeletingGlobalSetter = value;
    }

    ic(1);
    ic(2);
    expect(globalThis.globalSetterNeighbor).toBe("neighbor");
    expect(globalThis.selfDeletingGlobalSetter).toBe(2);
});
