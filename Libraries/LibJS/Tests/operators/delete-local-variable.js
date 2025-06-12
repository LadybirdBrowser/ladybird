test("basic functionality", () => {
    let a = 5;
    var b = 6;
    c = 7;
    expect(delete a).toBeFalse();
    expect(a).toBe(5);

    expect(delete b).toBeFalse();
    expect(b).toBe(6);

    expect(delete c).toBeTrue();

    expect(() => {
        c;
    }).toThrowWithMessage(ReferenceError, "'c' is not defined");
});
