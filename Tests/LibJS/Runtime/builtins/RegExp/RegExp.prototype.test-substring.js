describe("testing substrings without copying their characters", () => {
    for (const character of ["a", "é", "Ā", "😀"]) {
        for (const RegExpClass of [RegExp, class extends RegExp {}]) {
            test(`${character}, ${RegExpClass === RegExp ? "RegExp" : "subclass"}`, () => {
                let source = character.repeat(1024);
                const matching = new RegExpClass("^(.)", "u");
                const nonMatching = new RegExpClass("^!");
                if (RegExpClass !== RegExp) {
                    Object.setPrototypeOf(matching, RegExp.prototype);
                    Object.setPrototypeOf(nonMatching, RegExp.prototype);
                }
                let string = source.substring(20);

                for (let i = 0; i < 8; ++i) {
                    expect(getStringExternalMemorySize(string)).toBe(0);
                    expect(matching.test(string)).toBeTrue();
                    expect(getStringExternalMemorySize(string)).toBe(0);
                    if (RegExpClass === RegExp) expect(RegExp.$1).toBe(character);
                    expect(nonMatching.test(string)).toBeFalse();
                    expect(getStringExternalMemorySize(string)).toBe(0);
                    string = string.substring(20);
                }

                expect(getStringExternalMemorySize(source)).toBeGreaterThan(0);
                source = null;
                gc();
                expect(matching.test(string)).toBeTrue();
                expect(string.startsWith(character)).toBeTrue();
            });
        }
    }
});
