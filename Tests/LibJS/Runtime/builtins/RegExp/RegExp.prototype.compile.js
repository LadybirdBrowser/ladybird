test("basic functionality", () => {
    let re = /foo/;
    expect(re.compile.length).toBe(2);

    re.compile("bar");
    expect(re.test("foo")).toBeFalse();
    expect(re.test("bar")).toBeTrue();

    expect(re.unicode).toBeFalse();
    re.compile("bar", "u");
    expect(re.unicode).toBeTrue();

    re.compile(/baz/g);
    expect(re.global).toBeTrue();
    expect(re.test("bar")).toBeFalse();
    expect(re.test("baz")).toBeTrue();
});

test("reentrant initialization does not retain a stale matcher", () => {
    const regexp = /^ALLOW$/;

    regexp.compile({
        toString() {
            regexp.compile("^.*$");
            expect(regexp.test("BLOCKED")).toBeTrue();
            return "^ALLOW$";
        },
    });

    expect(regexp.source).toBe("^ALLOW$");
    expect(regexp.test("BLOCKED")).toBeFalse();
});
