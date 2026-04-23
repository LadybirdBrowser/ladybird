test("length is 0", () => {
    expect(Array.prototype.shift).toHaveLength(0);
});

describe("normal behavior", () => {
    test("array with elements", () => {
        var a = [1, 2, 3];
        expect(a.shift()).toBe(1);
        expect(a).toEqual([2, 3]);
    });

    test("empty array", () => {
        var a = [];
        expect(a.shift()).toBeUndefined();
        expect(a).toEqual([]);
    });

    test("array with empty slot", () => {
        var a = [,];
        expect(a.shift()).toBeUndefined();
        expect(a).toEqual([]);
    });
});

test("Issue #5884, GenericIndexedPropertyStorage::take_first() loses elements", () => {
    const a = [];
    for (let i = 0; i < 300; i++) {
        // NOTE: We use defineProperty to prevent the array from using SimpleIndexedPropertyStorage
        Object.defineProperty(a, i, { value: i, configurable: true, writable: true });
    }
    expect(a.length).toBe(300);
    for (let i = 0; i < 300; i++) {
        a.shift();
    }
    expect(a.length).toBe(0);
});

test("throws if the array length is not writable", () => {
    var a = [1, 2];
    Object.defineProperty(a, "length", { writable: false });

    expect(() => {
        a.shift();
    }).toThrow(TypeError);
    expect(a[0]).toBe(2);
    expect(a[1]).toBeUndefined();
    expect(1 in a).toBeFalse();
    expect(a.length).toBe(2);
});

describe("holey arrays", () => {
    test("shift on clean holey array with interior hole preserves hole", () => {
        const a = [0, , 2];
        const result = a.shift();
        expect(result).toBe(0);
        expect(a.length).toBe(2);
        expect(0 in a).toBeFalse();
        expect(a[0]).toBeUndefined();
        expect(1 in a).toBeTrue();
        expect(a[1]).toBe(2);
    });

    test("shift on clean holey array with leading hole returns undefined", () => {
        const a = [, 1, 2];
        const result = a.shift();
        expect(result).toBeUndefined();
        expect(a.length).toBe(2);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
    });

    test("shift on clean holey array with trailing hole propagates it", () => {
        const a = [0, 1, ,];
        expect(a.length).toBe(3);
        const result = a.shift();
        expect(result).toBe(0);
        expect(a.length).toBe(2);
        expect(a[0]).toBe(1);
        expect(1 in a).toBeFalse();
    });

    test("holey shift with prototype pollution follows spec", () => {
        Array.prototype[1] = "polluted";
        try {
            const a = [0, , 2];
            const result = a.shift();
            expect(result).toBe(0);
            // Spec: HasProperty(a, 1) is true via proto, Get returns "polluted",
            // which is set as an own property at index 0.
            expect(a[0]).toBe("polluted");
            expect(0 in a).toBeTrue();
            expect(a[1]).toBe(2);
            expect(a.length).toBe(2);
        } finally {
            delete Array.prototype[1];
        }
    });

    test("holey shift on non-extensible array throws TypeError", () => {
        const a = [, 1];
        Object.preventExtensions(a);
        expect(() => a.shift()).toThrow(TypeError);
    });

    test("packed shift with prototype pollution still fast-paths correctly", () => {
        Array.prototype[0] = "polluted";
        try {
            const a = [1, 2, 3];
            expect(a.shift()).toBe(1);
            expect(a).toEqual([2, 3]);
        } finally {
            delete Array.prototype[0];
        }
    });

    test("packed shift on non-extensible array works (no new properties)", () => {
        const a = [1, 2, 3];
        Object.preventExtensions(a);
        expect(a.shift()).toBe(1);
        expect(a.length).toBe(2);
        expect(a[0]).toBe(2);
        expect(a[1]).toBe(3);
    });
});
