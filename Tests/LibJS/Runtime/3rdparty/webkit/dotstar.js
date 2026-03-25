test("dotstar", () => {
    // WebKit assertion compatibility shim for Ladybird's test-js harness

    function description(msg) {
        // No-op, just used for test documentation in WebKit.
    }

    function shouldBe(actual_code, expected_code) {
        let actual = eval(actual_code);
        let expected = eval(expected_code);
        if (typeof actual === "string" && typeof expected === "string") {
            expect(actual).toBe(expected);
        } else if (Array.isArray(actual) && Array.isArray(expected)) {
            expect(actual).toEqual(expected);
        } else if (actual !== null && typeof actual === "object" && expected !== null && typeof expected === "object") {
            expect(actual).toEqual(expected);
        } else {
            expect(actual).toBe(expected);
        }
    }

    function shouldBeTrue(code) {
        expect(eval(code)).toBeTrue();
    }

    function shouldBeFalse(code) {
        expect(eval(code)).toBeFalse();
    }

    function shouldBeNull(code) {
        expect(eval(code)).toBeNull();
    }

    function shouldBeUndefined(code) {
        expect(eval(code)).toBeUndefined();
    }

    function shouldThrow(code, expected_error) {
        expect(() => eval(code)).toThrow();
    }

    function shouldNotThrow(code) {
        eval(code);
    }

    description("This page tests handling of parentheses subexpressions.");

    var regexp1 = /.*blah.*/;
    shouldBeNull("regexp1.exec('test')");
    shouldBe("regexp1.exec('blah')", "['blah']");
    shouldBe("regexp1.exec('1blah')", "['1blah']");
    shouldBe("regexp1.exec('blah1')", "['blah1']");
    shouldBe("regexp1.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp1.exec('blah\\nsecond')", "['blah']");
    shouldBe("regexp1.exec('first\\nblah')", "['blah']");
    shouldBe("regexp1.exec('first\\nblah\\nthird')", "['blah']");
    shouldBe("regexp1.exec('first\\nblah2\\nblah3')", "['blah2']");

    var regexp2 = /^.*blah.*/;
    shouldBeNull("regexp2.exec('test')");
    shouldBe("regexp2.exec('blah')", "['blah']");
    shouldBe("regexp2.exec('1blah')", "['1blah']");
    shouldBe("regexp2.exec('blah1')", "['blah1']");
    shouldBe("regexp2.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp2.exec('blah\\nsecond')", "['blah']");
    shouldBeNull("regexp2.exec('first\\nblah')");
    shouldBeNull("regexp2.exec('first\\nblah\\nthird')");
    shouldBeNull("regexp2.exec('first\\nblah2\\nblah3')");

    var regexp3 = /.*blah.*$/;
    shouldBeNull("regexp3.exec('test')");
    shouldBe("regexp3.exec('blah')", "['blah']");
    shouldBe("regexp3.exec('1blah')", "['1blah']");
    shouldBe("regexp3.exec('blah1')", "['blah1']");
    shouldBe("regexp3.exec('blah blah blah')", "['blah blah blah']");
    shouldBeNull("regexp3.exec('blah\\nsecond')");
    shouldBe("regexp3.exec('first\\nblah')", "['blah']");
    shouldBeNull("regexp3.exec('first\\nblah\\nthird')");
    shouldBe("regexp3.exec('first\\nblah2\\nblah3')", "['blah3']");

    var regexp4 = /^.*blah.*$/;
    shouldBeNull("regexp4.exec('test')");
    shouldBe("regexp4.exec('blah')", "['blah']");
    shouldBe("regexp4.exec('1blah')", "['1blah']");
    shouldBe("regexp4.exec('blah1')", "['blah1']");
    shouldBe("regexp4.exec('blah blah blah')", "['blah blah blah']");
    shouldBeNull("regexp4.exec('blah\\nsecond')");
    shouldBeNull("regexp4.exec('first\\nblah')");
    shouldBeNull("regexp4.exec('first\\nblah\\nthird')");
    shouldBeNull("regexp4.exec('first\\nblah2\\nblah3')");

    var regexp5 = /.*?blah.*/;
    shouldBeNull("regexp5.exec('test')");
    shouldBe("regexp5.exec('blah')", "['blah']");
    shouldBe("regexp5.exec('1blah')", "['1blah']");
    shouldBe("regexp5.exec('blah1')", "['blah1']");
    shouldBe("regexp5.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp5.exec('blah\\nsecond')", "['blah']");
    shouldBe("regexp5.exec('first\\nblah')", "['blah']");
    shouldBe("regexp5.exec('first\\nblah\\nthird')", "['blah']");
    shouldBe("regexp5.exec('first\\nblah2\\nblah3')", "['blah2']");

    var regexp6 = /.*blah.*?/;
    shouldBeNull("regexp6.exec('test')");
    shouldBe("regexp6.exec('blah')", "['blah']");
    shouldBe("regexp6.exec('1blah')", "['1blah']");
    shouldBe("regexp6.exec('blah1')", "['blah']");
    shouldBe("regexp6.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp6.exec('blah\\nsecond')", "['blah']");
    shouldBe("regexp6.exec('first\\nblah')", "['blah']");
    shouldBe("regexp6.exec('first\\nblah\\nthird')", "['blah']");
    shouldBe("regexp6.exec('first\\nblah2\\nblah3')", "['blah']");

    var regexp7 = /^.*?blah.*?$/;
    shouldBeNull("regexp7.exec('test')");
    shouldBe("regexp7.exec('blah')", "['blah']");
    shouldBe("regexp7.exec('1blah')", "['1blah']");
    shouldBe("regexp7.exec('blah1')", "['blah1']");
    shouldBe("regexp7.exec('blah blah blah')", "['blah blah blah']");
    shouldBeNull("regexp7.exec('blah\\nsecond')");
    shouldBeNull("regexp7.exec('first\\nblah')");
    shouldBeNull("regexp7.exec('first\\nblah\\nthird')");
    shouldBeNull("regexp7.exec('first\\nblah2\\nblah3')");

    var regexp8 = /^(.*)blah.*$/;
    shouldBeNull("regexp8.exec('test')");
    shouldBe("regexp8.exec('blah')", "['blah','']");
    shouldBe("regexp8.exec('1blah')", "['1blah','1']");
    shouldBe("regexp8.exec('blah1')", "['blah1','']");
    shouldBe("regexp8.exec('blah blah blah')", "['blah blah blah','blah blah ']");
    shouldBeNull("regexp8.exec('blah\\nsecond')");
    shouldBeNull("regexp8.exec('first\\nblah')");
    shouldBeNull("regexp8.exec('first\\nblah\\nthird')");
    shouldBeNull("regexp8.exec('first\\nblah2\\nblah3')");

    var regexp9 = /.*blah.*/m;
    shouldBeNull("regexp9.exec('test')");
    shouldBe("regexp9.exec('blah')", "['blah']");
    shouldBe("regexp9.exec('1blah')", "['1blah']");
    shouldBe("regexp9.exec('blah1')", "['blah1']");
    shouldBe("regexp9.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp9.exec('blah\\nsecond')", "['blah']");
    shouldBe("regexp9.exec('first\\nblah')", "['blah']");
    shouldBe("regexp9.exec('first\\nblah\\nthird')", "['blah']");
    shouldBe("regexp9.exec('first\\nblah2\\nblah3')", "['blah2']");

    var regexp10 = /^.*blah.*/m;
    shouldBeNull("regexp10.exec('test')");
    shouldBe("regexp10.exec('blah')", "['blah']");
    shouldBe("regexp10.exec('1blah')", "['1blah']");
    shouldBe("regexp10.exec('blah1')", "['blah1']");
    shouldBe("regexp10.exec('blah blah blah')", "['blah blah blah']");
    shouldBe("regexp10.exec('blah\\nsecond')", "['blah']");
    shouldBe("regexp10.exec('first\\nblah')", "['blah']");
    shouldBe("regexp10.exec('first\\nblah\\nthird')", "['blah']");
    shouldBe("regexp10.exec('first\\nblah2\\nblah3')", "['blah2']");

    var regexp11 = /.*(?:blah).*$/;
    shouldBeNull("regexp11.exec('test')");
    shouldBe("regexp11.exec('blah')", "['blah']");
    shouldBe("regexp11.exec('1blah')", "['1blah']");
    shouldBe("regexp11.exec('blah1')", "['blah1']");
    shouldBe("regexp11.exec('blah blah blah')", "['blah blah blah']");
    shouldBeNull("regexp11.exec('blah\\nsecond')");
    shouldBe("regexp11.exec('first\\nblah')", "['blah']");
    shouldBeNull("regexp11.exec('first\\nblah\\nthird')");
    shouldBe("regexp11.exec('first\\nblah2\\nblah3')", "['blah3']");

    var regexp12 = /.*(?:blah|buzz|bang).*$/;
    shouldBeNull("regexp12.exec('test')");
    shouldBe("regexp12.exec('blah')", "['blah']");
    shouldBe("regexp12.exec('1blah')", "['1blah']");
    shouldBe("regexp12.exec('blah1')", "['blah1']");
    shouldBe("regexp12.exec('blah blah blah')", "['blah blah blah']");
    shouldBeNull("regexp12.exec('blah\\nsecond')");
    shouldBe("regexp12.exec('first\\nblah')", "['blah']");
    shouldBeNull("regexp12.exec('first\\nblah\\nthird')");
    shouldBe("regexp12.exec('first\\nblah2\\nblah3')", "['blah3']");

    var regexp13 = /.*\n\d+.*/;
    shouldBe("regexp13.exec('abc\\n123')", "['abc\\n123']");

    var regexp14 = /.?d.*/;
    shouldBe("regexp14.exec('abcdefg')", "['cdefg']");

    var regexp15 = /.*d.?/;
    shouldBe("regexp15.exec('abcdefg')", "['abcde']");

    var regexp16 = /.?d.?/;
    shouldBe("regexp16.exec('abcdefg')", "['cde']");

    var regexp17 = /.{0,2}d.*/;
    shouldBe("regexp17.exec('abcdefg')", "['bcdefg']");

    var regexp18 = /.*d.{0,2}/;
    shouldBe("regexp18.exec('abcdefg')", "['abcdef']");

    var regexp19 = /.{0,2}d.{0,2}/;
    shouldBe("regexp19.exec('abcdefg')", "['bcdef']");
});
