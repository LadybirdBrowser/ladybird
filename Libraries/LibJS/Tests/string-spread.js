function testArray(arr) {
    return arr.length === 4 && arr[0] === "a" && arr[1] === "b" && arr[2] === "c" && arr[3] === "d";
}

test("spreading string literal", () => {
    expect(["a", ..."bc", "d"]).toEqual(["a", "b", "c", "d"]);
});

test("spreading string variable", () => {
    const s = "bc";
    expect(["a", ...s, "d"]).toEqual(["a", "b", "c", "d"]);
});

test("spreading string in object", () => {
    const obj = { a: "bc" };
    expect(["a", ...obj.a, "d"]).toEqual(["a", "b", "c", "d"]);
});

test("spreading empty string", () => {
    expect([..."", "a", ..."bc", ..."", "d", ...""]).toEqual(["a", "b", "c", "d"]);
});

test("spreading string objects", () => {
    expect([..."", ...[...new String("abc")], "d"]).toEqual(["a", "b", "c", "d"]);
});
