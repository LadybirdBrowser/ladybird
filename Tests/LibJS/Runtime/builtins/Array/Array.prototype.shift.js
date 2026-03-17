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
