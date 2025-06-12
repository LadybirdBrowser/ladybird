test("basic functionality", () => {
    let o = {
        foo: 1,
        bar: "baz",
        qux: {
            get x() {
                return 10;
            },
            y() {
                return 20;
            },
            arr: [1, 2, 3],
        },
    };

    let string = JSON.stringify(o, null, 4);
    let expected = `{
    "foo": 1,
    "bar": "baz",
    "qux": {
        "x": 10,
        "arr": [
            1,
            2,
            3
        ]
    }
}`;

    expect(string).toBe(expected);

    string = JSON.stringify(o, null, "abcd");
    expected = `{
abcd"foo": 1,
abcd"bar": "baz",
abcd"qux": {
abcdabcd"x": 10,
abcdabcd"arr": [
abcdabcdabcd1,
abcdabcdabcd2,
abcdabcdabcd3
abcdabcd]
abcd}
}`;

    expect(string).toBe(expected);
});
