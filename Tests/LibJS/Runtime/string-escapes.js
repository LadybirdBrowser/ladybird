test("hex escapes", () => {
    expect("\x55").toBe("U");
    expect("X55").toBe("X55");
    expect(`\x55`).toBe("U");
    expect(`\X55`).toBe("X55");
    expect("\xff").toBe(String.fromCharCode(0xff));
    expect("'\\x'").not.toEval();
    expect("'\\x1'").not.toEval();
    expect("'\\xz'").not.toEval();
    expect("'\\xzz'").not.toEval();
    expect("'\\x🐞'").not.toEval();
});

test("unicode escapes", () => {
    expect("\u26a0").toBe("⚠");
    expect(`\u26a0`).toBe("⚠");
    expect("\u{1f41e}").toBe("🐞");
    expect(`\u{1f41e}`).toBe("🐞");
    expect("\u00ff").toBe(String.fromCharCode(0xff));
    expect("'\\u'").not.toEval();
    expect("'\\u1'").not.toEval();
    expect("'\\uf'").not.toEval();
    expect("'\\u123'").not.toEval();
    expect("'\\u123z'").not.toEval();
    expect("'\\uz'").not.toEval();
    expect("'\\uzz'").not.toEval();
    expect("'\\uzzzz'").not.toEval();
    expect("'\\u{'").not.toEval();
    expect("'\\u{}'").not.toEval();
    expect("'\\u{z}'").not.toEval();
    expect("'\\u🐞'").not.toEval();
});

describe("octal escapes", () => {
    test("basic functionality", () => {
        expect("\1").toBe("\u0001");
        expect("\2").toBe("\u0002");
        expect("\3").toBe("\u0003");
        expect("\4").toBe("\u0004");
        expect("\5").toBe("\u0005");
        expect("\6").toBe("\u0006");
        expect("\7").toBe("\u0007");
        // prettier-ignore
        expect("\8").toBe("8");
        // prettier-ignore
        expect("\9").toBe("9");
        expect("\128").toBe("\n8");
        expect("\141bc").toBe("abc");
        expect("f\157o\142a\162").toBe("foobar");
        expect("\123\145\162\145\156\151\164\171\117\123").toBe("SerenityOS");
    });

    test("syntax error in template literal", () => {
        expect("`\\123`").not.toEval();
    });

    test("syntax error in strict mode", () => {
        expect("'use strict'; '\\123'").not.toEval();
        expect('"use strict"; "\\123"').not.toEval();
        // Special case, string literal precedes use strict directive
        expect("'\\123'; 'use strict'").not.toEval();
        // Because of the non string statement in the middle strict mode is not enabled.
        expect("'\\123'; somethingElse; 'use strict'").toEval();
    });

    test("invalid octal escapes fail in strict mode", () => {
        expect("'use strict'; '\\8'").not.toEval();
        expect("'use strict'; '\\800'").not.toEval();
        expect("'use strict'; '\\9'").not.toEval();
        expect("'use strict'; '\\912'").not.toEval();
    });
});
