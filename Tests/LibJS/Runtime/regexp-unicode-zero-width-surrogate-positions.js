for (const flag of ["u", "v"]) {
    test(`/${flag} zero-width searches skip surrogate interiors`, () => {
        const match = new RegExp("(?!😀)", flag).exec("😀");
        expect(match[0]).toBe("");
        expect(match.index).toBe(2);
        expect(new RegExp("(?!\\W)$", flag).test("😀")).toBeTrue();
        expect(new RegExp("(?!\\W)(?=.)", flag).test("😀")).toBeFalse();
    });

    for (const [source, indices, replacement] of [
        ["(?<g1>.{0}\\k<g1>)", [0, 2, 4, 6], "#😀#😀#😀#"],
        ["(?<a>)\\k<a>(?<!.)", [0], "#😀😀😀"],
        ["(?!\\W)", [6], "😀😀😀#"],
        ["(?<!.)", [0], "#😀😀😀"],
        ["\\B", [0, 2, 4, 6], "#😀#😀#😀#"],
    ]) {
        test(`/${source}/g${flag} matches only at code-point boundaries`, () => {
            const matcher = new RegExp(source, "g" + flag);
            expect(Array.from("😀😀😀".matchAll(matcher), match => match.index)).toEqual(indices);
            expect("😀😀😀".match(matcher)).toEqual(indices.map(() => ""));
            expect("😀😀😀".replace(matcher, "#")).toBe(replacement);
            expect("😀😀😀".replace(matcher, () => "#")).toBe(replacement);
        });
    }

    test(`/${flag} searches retain lone surrogates`, () => {
        const matcher = new RegExp("(?!😀)", "g" + flag);
        const input = "\uD83Da\uDE00😀";
        expect(Array.from(input.matchAll(matcher), match => match.index)).toEqual([0, 1, 2, 5]);
    });

    for (const source of ["\\B", "(?!😀)"]) {
        test(`/${source}/g${flag} searches past a pair containing lastIndex`, () => {
            const matcher = new RegExp(source, "dg" + flag);
            matcher.lastIndex = 2;
            const match = matcher.exec("A😀");
            expect(match[0]).toBe("");
            expect(match.index).toBe(3);
            expect(match.indices).toEqual([[3, 3]]);
            expect(matcher.lastIndex).toBe(3);
        });

        for (const flags of ["y" + flag, "gy" + flag]) {
            test(`/${source}/${flags} does not retry inside a surrogate pair`, () => {
                const matcher = new RegExp(source, flags);
                matcher.lastIndex = 2;
                expect(matcher.exec("A😀")).toBeNull();
                expect(matcher.lastIndex).toBe(0);

                matcher.lastIndex = 2;
                expect(matcher.test("A😀")).toBeFalse();
                expect(matcher.lastIndex).toBe(0);
            });
        }

        test(`/${source}/y${flag} replacement does not split a surrogate pair`, () => {
            const matcher = new RegExp(source, "y" + flag);
            matcher.lastIndex = 2;
            expect("A😀".replace(matcher, "#")).toBe("A😀");
            expect(matcher.lastIndex).toBe(0);

            matcher.lastIndex = 2;
            expect("A😀".replace(matcher, () => "#")).toBe("A😀");
            expect(matcher.lastIndex).toBe(0);
        });
    }

    for (const flags of ["g" + flag, "y" + flag]) {
        for (const [source, matched, end] of [
            ["(?:)", "", 1],
            [".", "😀", 3],
        ]) {
            test(`/${source}/${flags} starts at the code point containing lastIndex`, () => {
                const matcher = new RegExp(source, flags);
                matcher.lastIndex = 2;
                const match = matcher.exec("A😀");
                expect(match[0]).toBe(matched);
                expect(match.index).toBe(1);
                expect(matcher.lastIndex).toBe(end);
            });
        }
    }
}

test("non-Unicode zero-width patterns can match inside surrogate pairs", () => {
    expect(Array.from("😀".matchAll(/\B/g), match => match.index)).toEqual([0, 1, 2]);
    const matcher = /\B/y;
    matcher.lastIndex = 2;
    expect(matcher.exec("A😀").index).toBe(2);
    expect(matcher.lastIndex).toBe(2);
});
