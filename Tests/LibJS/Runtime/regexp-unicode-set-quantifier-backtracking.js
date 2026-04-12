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

test("mixed character class in lazy quantifier does not hit backtrack limit", () => {
    let str =
        "PYzJdxPYzJdxPYzJdx\t\ud83e\udd84\ud83c\udf0a\ud83d\udd25\t\u304b\u0330\u030a\u0307\t  \n\u65e5\u0332\u0300\u0304\ud83d\udc69\u200d\ud83c\udfa8\ud83d\udc68\u200d\ud83d\udd2c \n\u0433\u0306\u0305\u030b\ue0f9\n\u000b\t ;\n\f\n\u000b\n'+[-\ud83d\udc0d\ud83d\udc0d\ud83d\udc0d\ud83d\udc0d-\u3044\u0306\u0305-\ue567\u08fb\udfeb\ud814\udca0\ud814\udc9d";
    expect(() =>
        str.match(/[\x57-\x6cæ\0\SA-M\xa5]*?.{2}\uec19|(?<g1>(?<!\k<g1>))^Q+?\1🍕{0,}|(?<!\k<g1>)\d+?\u{f9bf5}/is)
    ).not.toThrow();
    expect(
        str.match(/[\x57-\x6cæ\0\SA-M\xa5]*?.{2}\uec19|(?<g1>(?<!\k<g1>))^Q+?\1🍕{0,}|(?<!\k<g1>)\d+?\u{f9bf5}/is)
    ).toBeNull();
});
