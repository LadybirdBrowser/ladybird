test("basic functionality", () => {
    expect(typeof "foo").toBe("string");
    expect(typeof (1 + 2)).toBe("number");
    expect(typeof {}).toBe("object");
    expect(typeof null).toBe("object");
    expect(typeof undefined).toBe("undefined");
    expect(typeof 1n).toBe("bigint");
    expect(typeof Symbol()).toBe("symbol");
    expect(typeof function () {}).toBe("function");

    var iExist = 1;
    expect(typeof iExist).toBe("number");
    expect(typeof iDontExist).toBe("undefined");
});

test("typeof calls property getter", () => {
    var calls = 0;
    Object.defineProperty(globalThis, "foo", {
        get() {
            calls++;
            return 10;
        },
    });

    expect(typeof foo).toBe("number");
    expect(calls).toBe(1);
});
