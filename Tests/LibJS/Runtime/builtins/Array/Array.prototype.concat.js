test("length is 1", () => {
    expect(Array.prototype.concat).toHaveLength(1);
});

describe("normal behavior", () => {
    var array = ["hello"];

    test("no arguments", () => {
        var concatenated = array.concat();
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(1);
    });

    test("single argument", () => {
        var concatenated = array.concat("friends");
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(2);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe("friends");
    });

    test("single array argument", () => {
        var concatenated = array.concat([1, 2, 3]);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(4);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe(1);
        expect(concatenated[2]).toBe(2);
        expect(concatenated[3]).toBe(3);
    });

    test("multiple arguments", () => {
        var concatenated = array.concat(false, "serenity", { name: "libjs" }, [1, [2, 3]]);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(6);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBeFalse();
        expect(concatenated[2]).toBe("serenity");
        expect(concatenated[3]).toEqual({ name: "libjs" });
        expect(concatenated[4]).toBe(1);
        expect(concatenated[5]).toEqual([2, 3]);
    });

    test("Proxy is concatenated as array", () => {
        var proxy = new Proxy([9, 8], {});
        var concatenated = array.concat(proxy);
        expect(array).toHaveLength(1);
        expect(concatenated).toHaveLength(3);
        expect(concatenated[0]).toBe("hello");
        expect(concatenated[1]).toBe(9);
        expect(concatenated[2]).toBe(8);
    });
});
