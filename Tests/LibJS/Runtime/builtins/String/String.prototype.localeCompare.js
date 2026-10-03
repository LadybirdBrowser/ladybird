test("basic functionality", () => {
    expect(String.prototype.localeCompare).toHaveLength(1);

    expect("".localeCompare("")).toBe(0);
    expect("a".localeCompare("a")).toBe(0);
    expect("6".localeCompare("6")).toBe(0);

    function compareBoth(a, b) {
        const aTob = a.localeCompare(b);
        const bToa = b.localeCompare(a);

        expect(aTob > 0).toBeTrue();
        expect(aTob).toBe(-bToa);
    }

    compareBoth("a", "");
    compareBoth("1", "");
    compareBoth("A", "a");
    compareBoth("7", "3");
    compareBoth("0000", "0");

    expect("undefined".localeCompare()).toBe(0);
    expect("undefined".localeCompare(undefined)).toBe(0);

    expect("null".localeCompare(null)).toBe(0);
    expect("null".localeCompare(undefined)).not.toBe(0);
    expect("null".localeCompare() < 0).toBeTrue();

    expect(() => {
        String.prototype.localeCompare.call(undefined, undefined);
    }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");
});

test("UTF-16", () => {
    var s = "😀😀";
    expect(s.localeCompare("😀😀")).toBe(0);
    expect(s.localeCompare("\ud83d") > 0);
    expect(s.localeCompare("😀😀s") < 0);
});

test("letter differences outweigh earlier case differences", () => {
    expect("ab".localeCompare("Aa")).toBe(1);
    expect("Aa".localeCompare("ab")).toBe(-1);
    expect("Abc".localeCompare("abd")).toBe(-1);
    expect("mcdonald".localeCompare("McAfee")).toBe(1);
    expect(["Banana", "apple", "banana", "Apple", "aPricot"].sort((a, b) => a.localeCompare(b))).toEqual([
        "apple",
        "Apple",
        "aPricot",
        "banana",
        "Banana",
    ]);
});

test("ignorable trailing characters", () => {
    expect("a\x01".localeCompare("a")).toBe(0);
    expect("a".localeCompare("a\x01")).toBe(0);
});

test("ASCII strings compare like the collator", () => {
    const collator = new Intl.Collator("en");
    const strings = ["", "a", "A", "b", "B", "ab", "Ab", "aB", "AB", "Aa", "a-b", "a b", "ab ", "a\x01", "1a", "A1"];
    for (const a of strings) {
        for (const b of strings) expect(a.localeCompare(b)).toBe(collator.compare(a, b));
    }
});
