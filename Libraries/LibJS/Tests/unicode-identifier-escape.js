test("basic escapes", () => {
    var foo = {};
    foo.brown = 12389;

    expect(foo.brown).toBe(12389);
    expect(foo.br\u006fwn).toBe(12389);
    expect(foo.br\u{6f}wn).toBe(12389);
    expect(foo.\u{62}\u{72}\u{6f}\u{77}\u{6e}).toBe(12389);
});

test("non-ascii escapes", () => {
    var foo = {};
    foo.𝓑𝓻𝓸𝔀𝓷 = 12389;

    expect(foo.𝓑𝓻𝓸𝔀𝓷).toBe(12389);
    expect(foo.𝓑𝓻\u{1d4f8}𝔀𝓷).toBe(12389);
    expect(foo.\u{1d4d1}\u{1d4fb}\u{1d4f8}\u{1d500}\u{1d4f7}).toBe(12389);

    // U-16 High surrogate pair is allowed in string but not in identifier.
    expect("foo.𝓑𝓻\ud835\udcf8𝔀𝓷").toEval();
    expect("foo.𝓑𝓻\\ud835\\udcf8𝔀𝓷").not.toEval();
});

describe("escaped keywords", () => {
    // We must double escape the slashes here else the strings already convert
    // the escaped characters (and string is more lenient).
    test("keywords cannot be used in an escaped form", () => {
        expect("\\u{69}\\u{66}(true) throw 'Should fail'").not.toEval();
        expect("wh\\u{69}le(true) throw 'Should fail'").not.toEval();

        expect("l\\u{65}t a = 3;").not.toEval();
        expect("function *G(){ yiel\\0064 3; }").not.toEval();
    });

    test("escaped keywords cannot be used as standalone variables", () => {
        expect("var fu\\u{6e}ction = 4").not.toEval();
        expect("var \\u0077ith = 4").not.toEval();
    });

    test("'yield' and 'let' can be escaped as variables", () => {
        var l\u{65}t = 3;
        var yi\u0065ld = 5;
        expect(let).toBe(3);
        expect(yield).toBe(5);
    });

    test("'let' cannot be used in a lexical declaration but 'yield' can", () => {
        expect("const l\\u{65}t = 3;").not.toEval();

        const yi\u0065ld = 5;
        expect(yield).toBe(5);
    });

    test("escaped 'yield' and 'let' variables are not allowed in strict mode", () => {
        expect("function f() { 'use strict'; var l\\u{65}t = 3; }").not.toEval();
        expect("function g() { 'use strict'; var yi\u0065ld = 5; }").not.toEval();
    });

    test("cannot use escaped 'yield' variable or label in generator context", () => {
        expect("function *g() { var yi\u0065ld = 5; }").not.toEval();
        expect("function *g() { yi\u0065ld: 5; }").not.toEval();
    });

    test("can use escaped 'let' variable and label in generator context", () => {
        expect("function *i() { var \\u{6c}et = 6; }").toEval();
        expect("function *j() { \\u{6c}et: 6; }").toEval();
    });

    test("can use keywords in some contexts", () => {
        var obj = {
            \u{69}\u{66}: 3,
            wh\u{69}le() {
                return 4;
            },
            ca\u0073e: "case",
            get true() {
                return false;
            },
        };

        expect(obj.\u{69}f).toBe(3);
        expect(obj.whi\u{6c}e()).toBe(4);
        expect(obj.\u{63}ase).toBe("case");
        expect(obj.\u0074r\u{0000075}e).toBeFalse();
    });
});
