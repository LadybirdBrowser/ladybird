test("/v mode negated class in lazy quantifier does not hit backtrack limit", () => {
    expect(() => "aplmaaaaacdeaabbaaqw".match(/([[^p][^p]]+)(.*).*apl/v)).not.toThrow();
    expect("aplmaaaaacdeaabbaaqw".match(/([[^p][^p]]+)(.*).*apl/v)).toBeNull();
});

test("/v mode union of range class and unicode property in lazy quantifier does not hit backtrack limit", () => {
    let str =
        "\u0436 \u0442\u0435\u0441\u0442\u5b57({foo \ud83d\ude10\ud83d\ude01\ud83d\ude01\ud83d\ude01 1\ufe0f\u20e34\ufe0f\u20e34\ufe0f\u20e3 = \u0939\u093f\u0928\u094d\u0926\u0940 \u05d5\u05d5;\u0917 @@@\u00d7\u00f7\u2211\u222b\u221e\u222b\u220f\u062f\u062a\n\u2728\u2728\u2728\u2728[=/\ud83c\udf0a\ud83d\udca8\u5c71\nb \u6f22\u5b57 ";
    expect(() =>
        str.match(/(?<g27>\P{Emoji}.(?:[[^😀-🚀][^\p{Emoji}]]+?))((?<n96>[[A-Z]]*?)*?).*\k<g27>/gv)
    ).not.toThrow();
    expect(str.match(/(?<g27>\P{Emoji}.(?:[[^😀-🚀][^\p{Emoji}]]+?))((?<n96>[[A-Z]]*?)*?).*\k<g27>/gv)).toEqual([
        "\ufe0f\u20e34\ufe0f\u20e34",
    ]);
});
