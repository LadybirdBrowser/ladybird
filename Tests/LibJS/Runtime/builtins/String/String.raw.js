test("basic functionality", () => {
    expect(String.raw).toHaveLength(1);

    let str = String.raw`foo\nbar`;
    expect(str).toHaveLength(8);
    expect(str).toBe("foo\\nbar");

    str = String.raw`foo ${1 + 9}\nbar${"hf!"}`;
    expect(str).toBe("foo 10\\nbarhf!");

    str = String.raw`${10}${20}${30}`;
    expect(str).toBe("102030");

    str = String.raw({ raw: ["foo ", "\\nbar"] }, 10, "hf!");
    expect(str).toBe("foo 10\\nbar");

    str = String.raw({ raw: ["foo ", "\\nbar"] });
    expect(str).toBe("foo \\nbar");

    str = String.raw({ raw: [] }, 10, "hf!");
    expect(str).toBe("");

    str = String.raw({ raw: 1 });
    expect(str).toBe("");
});

test("passing object with no 'raw' property", () => {
    expect(() => {
        String.raw({});
    }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
});
