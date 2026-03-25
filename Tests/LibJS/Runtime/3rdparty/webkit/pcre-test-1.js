test.skip("pcre-test-1", () => {
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

    description("A chunk of our port of PCRE's test suite, adapted to be more applicable to JavaScript.");

    var regex0 = /the quick brown fox/;
    var input0 = "the quick brown fox";
    var results = ["the quick brown fox"];
    shouldBe("regex0.exec(input0);", "results");
    var input1 = "The quick brown FOX";
    var results = null;
    shouldBe("regex0.exec(input1);", "results");
    var input2 = "What do you know about the quick brown fox?";
    var results = ["the quick brown fox"];
    shouldBe("regex0.exec(input2);", "results");
    var input3 = "What do you know about THE QUICK BROWN FOX?";
    var results = null;
    shouldBe("regex0.exec(input3);", "results");

    var regex1 = /The quick brown fox/i;
    var input0 = "the quick brown fox";
    var results = ["the quick brown fox"];
    shouldBe("regex1.exec(input0);", "results");
    var input1 = "The quick brown FOX";
    var results = ["The quick brown FOX"];
    shouldBe("regex1.exec(input1);", "results");
    var input2 = "What do you know about the quick brown fox?";
    var results = ["the quick brown fox"];
    shouldBe("regex1.exec(input2);", "results");
    var input3 = "What do you know about THE QUICK BROWN FOX?";
    var results = ["THE QUICK BROWN FOX"];
    shouldBe("regex1.exec(input3);", "results");

    var regex2 = /abcd\t\n\r\f\v[\b]\071\x3b\$\\\?caxyz/;
    var input0 = "abcd\t\n\r\f\v\b9;\$\\?caxyz";
    var results = ["abcd\x09\x0a\x0d\x0c\x0b\x089;$\\?caxyz"];
    shouldBe("regex2.exec(input0);", "results");

    var regex3 = /a*abc?xyz+pqr{3}ab{2,}xy{4,5}pq{0,6}AB{0,}zz/;
    var input0 = "abxyzpqrrrabbxyyyypqAzz";
    var results = ["abxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input0);", "results");
    var input1 = "abxyzpqrrrabbxyyyypqAzz";
    var results = ["abxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input1);", "results");
    var input2 = "aabxyzpqrrrabbxyyyypqAzz";
    var results = ["aabxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input2);", "results");
    var input3 = "aaabxyzpqrrrabbxyyyypqAzz";
    var results = ["aaabxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input3);", "results");
    var input4 = "aaaabxyzpqrrrabbxyyyypqAzz";
    var results = ["aaaabxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input4);", "results");
    var input5 = "abcxyzpqrrrabbxyyyypqAzz";
    var results = ["abcxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input5);", "results");
    var input6 = "aabcxyzpqrrrabbxyyyypqAzz";
    var results = ["aabcxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input6);", "results");
    var input7 = "aaabcxyzpqrrrabbxyyyypAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypAzz"];
    shouldBe("regex3.exec(input7);", "results");
    var input8 = "aaabcxyzpqrrrabbxyyyypqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input8);", "results");
    var input9 = "aaabcxyzpqrrrabbxyyyypqqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqqAzz"];
    shouldBe("regex3.exec(input9);", "results");
    var input10 = "aaabcxyzpqrrrabbxyyyypqqqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqqqAzz"];
    shouldBe("regex3.exec(input10);", "results");
    var input11 = "aaabcxyzpqrrrabbxyyyypqqqqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqqqqAzz"];
    shouldBe("regex3.exec(input11);", "results");
    var input12 = "aaabcxyzpqrrrabbxyyyypqqqqqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqqqqqAzz"];
    shouldBe("regex3.exec(input12);", "results");
    var input13 = "aaabcxyzpqrrrabbxyyyypqqqqqqAzz";
    var results = ["aaabcxyzpqrrrabbxyyyypqqqqqqAzz"];
    shouldBe("regex3.exec(input13);", "results");
    var input14 = "aaaabcxyzpqrrrabbxyyyypqAzz";
    var results = ["aaaabcxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input14);", "results");
    var input15 = "abxyzzpqrrrabbxyyyypqAzz";
    var results = ["abxyzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input15);", "results");
    var input16 = "aabxyzzzpqrrrabbxyyyypqAzz";
    var results = ["aabxyzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input16);", "results");
    var input17 = "aaabxyzzzzpqrrrabbxyyyypqAzz";
    var results = ["aaabxyzzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input17);", "results");
    var input18 = "aaaabxyzzzzpqrrrabbxyyyypqAzz";
    var results = ["aaaabxyzzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input18);", "results");
    var input19 = "abcxyzzpqrrrabbxyyyypqAzz";
    var results = ["abcxyzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input19);", "results");
    var input20 = "aabcxyzzzpqrrrabbxyyyypqAzz";
    var results = ["aabcxyzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input20);", "results");
    var input21 = "aaabcxyzzzzpqrrrabbxyyyypqAzz";
    var results = ["aaabcxyzzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input21);", "results");
    var input22 = "aaaabcxyzzzzpqrrrabbxyyyypqAzz";
    var results = ["aaaabcxyzzzzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input22);", "results");
    var input23 = "aaaabcxyzzzzpqrrrabbbxyyyypqAzz";
    var results = ["aaaabcxyzzzzpqrrrabbbxyyyypqAzz"];
    shouldBe("regex3.exec(input23);", "results");
    var input24 = "aaaabcxyzzzzpqrrrabbbxyyyyypqAzz";
    var results = ["aaaabcxyzzzzpqrrrabbbxyyyyypqAzz"];
    shouldBe("regex3.exec(input24);", "results");
    var input25 = "aaabcxyzpqrrrabbxyyyypABzz";
    var results = ["aaabcxyzpqrrrabbxyyyypABzz"];
    shouldBe("regex3.exec(input25);", "results");
    var input26 = "aaabcxyzpqrrrabbxyyyypABBzz";
    var results = ["aaabcxyzpqrrrabbxyyyypABBzz"];
    shouldBe("regex3.exec(input26);", "results");
    var input27 = ">>>aaabxyzpqrrrabbxyyyypqAzz";
    var results = ["aaabxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input27);", "results");
    var input28 = ">aaaabxyzpqrrrabbxyyyypqAzz";
    var results = ["aaaabxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input28);", "results");
    var input29 = ">>>>abcxyzpqrrrabbxyyyypqAzz";
    var results = ["abcxyzpqrrrabbxyyyypqAzz"];
    shouldBe("regex3.exec(input29);", "results");
    // Failers
    var input30 = "abxyzpqrrabbxyyyypqAzz";
    var results = null;
    shouldBe("regex3.exec(input30);", "results");
    var input31 = "abxyzpqrrrrabbxyyyypqAzz";
    var results = null;
    shouldBe("regex3.exec(input31);", "results");
    var input32 = "abxyzpqrrrabxyyyypqAzz";
    var results = null;
    shouldBe("regex3.exec(input32);", "results");
    var input33 = "aaaabcxyzzzzpqrrrabbbxyyyyyypqAzz";
    var results = null;
    shouldBe("regex3.exec(input33);", "results");
    var input34 = "aaaabcxyzzzzpqrrrabbbxyyypqAzz";
    var results = null;
    shouldBe("regex3.exec(input34);", "results");
    var input35 = "aaabcxyzpqrrrabbxyyyypqqqqqqqAzz";
    var results = null;
    shouldBe("regex3.exec(input35);", "results");

    var regex4 = /^(abc){1,2}zz/;
    var input0 = "abczz";
    var results = ["abczz", "abc"];
    shouldBe("regex4.exec(input0);", "results");
    var input1 = "abcabczz";
    var results = ["abcabczz", "abc"];
    shouldBe("regex4.exec(input1);", "results");
    // Failers
    var input2 = "zz";
    var results = null;
    shouldBe("regex4.exec(input2);", "results");
    var input3 = "abcabcabczz";
    var results = null;
    shouldBe("regex4.exec(input3);", "results");
    var input4 = ">>abczz";
    var results = null;
    shouldBe("regex4.exec(input4);", "results");

    var regex5 = /^(b+?|a){1,2}?c/;
    var input0 = "bc";
    var results = ["bc", "b"];
    shouldBe("regex5.exec(input0);", "results");
    var input1 = "bbc";
    var results = ["bbc", "b"];
    shouldBe("regex5.exec(input1);", "results");
    var input2 = "bbbc";
    var results = ["bbbc", "bb"];
    shouldBe("regex5.exec(input2);", "results");
    var input3 = "bac";
    var results = ["bac", "a"];
    shouldBe("regex5.exec(input3);", "results");
    var input4 = "bbac";
    var results = ["bbac", "a"];
    shouldBe("regex5.exec(input4);", "results");
    var input5 = "aac";
    var results = ["aac", "a"];
    shouldBe("regex5.exec(input5);", "results");
    var input6 = "abbbbbbbbbbbc";
    var results = ["abbbbbbbbbbbc", "bbbbbbbbbbb"];
    shouldBe("regex5.exec(input6);", "results");
    var input7 = "bbbbbbbbbbbac";
    var results = ["bbbbbbbbbbbac", "a"];
    shouldBe("regex5.exec(input7);", "results");
    // Failers
    var input8 = "aaac";
    var results = null;
    shouldBe("regex5.exec(input8);", "results");
    var input9 = "abbbbbbbbbbbac";
    var results = null;
    shouldBe("regex5.exec(input9);", "results");

    var regex6 = /^(b+|a){1,2}c/;
    var input0 = "bc";
    var results = ["bc", "b"];
    shouldBe("regex6.exec(input0);", "results");
    var input1 = "bbc";
    var results = ["bbc", "bb"];
    shouldBe("regex6.exec(input1);", "results");
    var input2 = "bbbc";
    var results = ["bbbc", "bbb"];
    shouldBe("regex6.exec(input2);", "results");
    var input3 = "bac";
    var results = ["bac", "a"];
    shouldBe("regex6.exec(input3);", "results");
    var input4 = "bbac";
    var results = ["bbac", "a"];
    shouldBe("regex6.exec(input4);", "results");
    var input5 = "aac";
    var results = ["aac", "a"];
    shouldBe("regex6.exec(input5);", "results");
    var input6 = "abbbbbbbbbbbc";
    var results = ["abbbbbbbbbbbc", "bbbbbbbbbbb"];
    shouldBe("regex6.exec(input6);", "results");
    var input7 = "bbbbbbbbbbbac";
    var results = ["bbbbbbbbbbbac", "a"];
    shouldBe("regex6.exec(input7);", "results");
    // Failers
    var input8 = "aaac";
    var results = null;
    shouldBe("regex6.exec(input8);", "results");
    var input9 = "abbbbbbbbbbbac";
    var results = null;
    shouldBe("regex6.exec(input9);", "results");

    var regex7 = /^(b+|a){1,2}?bc/;
    var input0 = "bbc";
    var results = ["bbc", "b"];
    shouldBe("regex7.exec(input0);", "results");

    var regex8 = /^(b*|ba){1,2}?bc/;
    var input0 = "babc";
    var results = ["babc", "ba"];
    shouldBe("regex8.exec(input0);", "results");
    var input1 = "bbabc";
    var results = ["bbabc", "ba"];
    shouldBe("regex8.exec(input1);", "results");
    var input2 = "bababc";
    var results = ["bababc", "ba"];
    shouldBe("regex8.exec(input2);", "results");
    // Failers
    var input3 = "bababbc";
    var results = null;
    shouldBe("regex8.exec(input3);", "results");
    var input4 = "babababc";
    var results = null;
    shouldBe("regex8.exec(input4);", "results");

    var regex9 = /^(ba|b*){1,2}?bc/;
    var input0 = "babc";
    var results = ["babc", "ba"];
    shouldBe("regex9.exec(input0);", "results");
    var input1 = "bbabc";
    var results = ["bbabc", "ba"];
    shouldBe("regex9.exec(input1);", "results");
    var input2 = "bababc";
    var results = ["bababc", "ba"];
    shouldBe("regex9.exec(input2);", "results");
    // Failers
    var input3 = "bababbc";
    var results = null;
    shouldBe("regex9.exec(input3);", "results");
    var input4 = "babababc";
    var results = null;
    shouldBe("regex9.exec(input4);", "results");

    var regex10 = /^\ca\cA/;
    var input0 = "\x01\x01";
    var results = ["\x01\x01"];
    shouldBe("regex10.exec(input0);", "results");

    var regex11 = /^[ab\]cde]/;
    var input0 = "athing";
    var results = ["a"];
    shouldBe("regex11.exec(input0);", "results");
    var input1 = "bthing";
    var results = ["b"];
    shouldBe("regex11.exec(input1);", "results");
    var input2 = "]thing";
    var results = ["]"];
    shouldBe("regex11.exec(input2);", "results");
    var input3 = "cthing";
    var results = ["c"];
    shouldBe("regex11.exec(input3);", "results");
    var input4 = "dthing";
    var results = ["d"];
    shouldBe("regex11.exec(input4);", "results");
    var input5 = "ething";
    var results = ["e"];
    shouldBe("regex11.exec(input5);", "results");
    // Failers
    var input6 = "fthing";
    var results = null;
    shouldBe("regex11.exec(input6);", "results");
    var input7 = "[thing";
    var results = null;
    shouldBe("regex11.exec(input7);", "results");
    var input8 = "\\thing";
    var results = null;
    shouldBe("regex11.exec(input8);", "results");

    var regex12 = /^[\]cde]/;
    var input0 = "]thing";
    var results = ["]"];
    shouldBe("regex12.exec(input0);", "results");
    var input1 = "cthing";
    var results = ["c"];
    shouldBe("regex12.exec(input1);", "results");
    var input2 = "dthing";
    var results = ["d"];
    shouldBe("regex12.exec(input2);", "results");
    var input3 = "ething";
    var results = ["e"];
    shouldBe("regex12.exec(input3);", "results");
    // Failers
    var input4 = "athing";
    var results = null;
    shouldBe("regex12.exec(input4);", "results");
    var input5 = "fthing";
    var results = null;
    shouldBe("regex12.exec(input5);", "results");

    var regex13 = /^[^ab\]cde]/;
    var input0 = "fthing";
    var results = ["f"];
    shouldBe("regex13.exec(input0);", "results");
    var input1 = "[thing";
    var results = ["["];
    shouldBe("regex13.exec(input1);", "results");
    var input2 = "\\thing";
    var results = ["\\"];
    shouldBe("regex13.exec(input2);", "results");
    // Failers
    var input3 = "athing";
    var results = null;
    shouldBe("regex13.exec(input3);", "results");
    var input4 = "bthing";
    var results = null;
    shouldBe("regex13.exec(input4);", "results");
    var input5 = "]thing";
    var results = null;
    shouldBe("regex13.exec(input5);", "results");
    var input6 = "cthing";
    var results = null;
    shouldBe("regex13.exec(input6);", "results");
    var input7 = "dthing";
    var results = null;
    shouldBe("regex13.exec(input7);", "results");
    var input8 = "ething";
    var results = null;
    shouldBe("regex13.exec(input8);", "results");

    var regex14 = /^[^\]cde]/;
    var input0 = "athing";
    var results = ["a"];
    shouldBe("regex14.exec(input0);", "results");
    var input1 = "fthing";
    var results = ["f"];
    shouldBe("regex14.exec(input1);", "results");
    // Failers
    var input2 = "]thing";
    var results = null;
    shouldBe("regex14.exec(input2);", "results");
    var input3 = "cthing";
    var results = null;
    shouldBe("regex14.exec(input3);", "results");
    var input4 = "dthing";
    var results = null;
    shouldBe("regex14.exec(input4);", "results");
    var input5 = "ething";
    var results = null;
    shouldBe("regex14.exec(input5);", "results");

    var regex15 = /^\xc2/;
    var input0 = "\xc2";
    var results = ["\xc2"];
    shouldBe("regex15.exec(input0);", "results");

    var regex16 = /^\xc3/;
    var input0 = "\xc3";
    var results = ["\xc3"];
    shouldBe("regex16.exec(input0);", "results");

    var regex17 = /^[0-9]+$/;
    var input0 = "0";
    var results = ["0"];
    shouldBe("regex17.exec(input0);", "results");
    var input1 = "1";
    var results = ["1"];
    shouldBe("regex17.exec(input1);", "results");
    var input2 = "2";
    var results = ["2"];
    shouldBe("regex17.exec(input2);", "results");
    var input3 = "3";
    var results = ["3"];
    shouldBe("regex17.exec(input3);", "results");
    var input4 = "4";
    var results = ["4"];
    shouldBe("regex17.exec(input4);", "results");
    var input5 = "5";
    var results = ["5"];
    shouldBe("regex17.exec(input5);", "results");
    var input6 = "6";
    var results = ["6"];
    shouldBe("regex17.exec(input6);", "results");
    var input7 = "7";
    var results = ["7"];
    shouldBe("regex17.exec(input7);", "results");
    var input8 = "8";
    var results = ["8"];
    shouldBe("regex17.exec(input8);", "results");
    var input9 = "9";
    var results = ["9"];
    shouldBe("regex17.exec(input9);", "results");
    var input10 = "10";
    var results = ["10"];
    shouldBe("regex17.exec(input10);", "results");
    var input11 = "100";
    var results = ["100"];
    shouldBe("regex17.exec(input11);", "results");
    // Failers
    var input12 = "abc";
    var results = null;
    shouldBe("regex17.exec(input12);", "results");

    var regex18 = /^.*nter/;
    var input0 = "enter";
    var results = ["enter"];
    shouldBe("regex18.exec(input0);", "results");
    var input1 = "inter";
    var results = ["inter"];
    shouldBe("regex18.exec(input1);", "results");
    var input2 = "uponter";
    var results = ["uponter"];
    shouldBe("regex18.exec(input2);", "results");

    var regex19 = /^xxx[0-9]+$/;
    var input0 = "xxx0";
    var results = ["xxx0"];
    shouldBe("regex19.exec(input0);", "results");
    var input1 = "xxx1234";
    var results = ["xxx1234"];
    shouldBe("regex19.exec(input1);", "results");
    // Failers
    var input2 = "xxx";
    var results = null;
    shouldBe("regex19.exec(input2);", "results");

    var regex20 = /^.+[0-9][0-9][0-9]$/;
    var input0 = "x123";
    var results = ["x123"];
    shouldBe("regex20.exec(input0);", "results");
    var input1 = "xx123";
    var results = ["xx123"];
    shouldBe("regex20.exec(input1);", "results");
    var input2 = "123456";
    var results = ["123456"];
    shouldBe("regex20.exec(input2);", "results");
    // Failers
    var input3 = "123";
    var results = null;
    shouldBe("regex20.exec(input3);", "results");
    var input4 = "x1234";
    var results = ["x1234"];
    shouldBe("regex20.exec(input4);", "results");

    var regex21 = /^.+?[0-9][0-9][0-9]$/;
    var input0 = "x123";
    var results = ["x123"];
    shouldBe("regex21.exec(input0);", "results");
    var input1 = "xx123";
    var results = ["xx123"];
    shouldBe("regex21.exec(input1);", "results");
    var input2 = "123456";
    var results = ["123456"];
    shouldBe("regex21.exec(input2);", "results");
    // Failers
    var input3 = "123";
    var results = null;
    shouldBe("regex21.exec(input3);", "results");
    var input4 = "x1234";
    var results = ["x1234"];
    shouldBe("regex21.exec(input4);", "results");

    var regex22 = /^([^!]+)!(.+)=apquxz\.ixr\.zzz\.ac\.uk$/;
    var input0 = "abc!pqr=apquxz.ixr.zzz.ac.uk";
    var results = ["abc!pqr=apquxz.ixr.zzz.ac.uk", "abc", "pqr"];
    shouldBe("regex22.exec(input0);", "results");
    // Failers
    var input1 = "!pqr=apquxz.ixr.zzz.ac.uk";
    var results = null;
    shouldBe("regex22.exec(input1);", "results");
    var input2 = "abc!=apquxz.ixr.zzz.ac.uk";
    var results = null;
    shouldBe("regex22.exec(input2);", "results");
    var input3 = "abc!pqr=apquxz:ixr.zzz.ac.uk";
    var results = null;
    shouldBe("regex22.exec(input3);", "results");
    var input4 = "abc!pqr=apquxz.ixr.zzz.ac.ukk";
    var results = null;
    shouldBe("regex22.exec(input4);", "results");

    var regex23 = /:/;
    var input0 = "Well, we need a colon: somewhere";
    var results = [":"];
    shouldBe("regex23.exec(input0);", "results");
    var input1 = "*** Fail if we don't";
    var results = null;
    shouldBe("regex23.exec(input1);", "results");

    var regex24 = /([\da-f:]+)$/i;
    var input0 = "0abc";
    var results = ["0abc", "0abc"];
    shouldBe("regex24.exec(input0);", "results");
    var input1 = "abc";
    var results = ["abc", "abc"];
    shouldBe("regex24.exec(input1);", "results");
    var input2 = "fed";
    var results = ["fed", "fed"];
    shouldBe("regex24.exec(input2);", "results");
    var input3 = "E";
    var results = ["E", "E"];
    shouldBe("regex24.exec(input3);", "results");
    var input4 = "::";
    var results = ["::", "::"];
    shouldBe("regex24.exec(input4);", "results");
    var input5 = "5f03:12C0::932e";
    var results = ["5f03:12C0::932e", "5f03:12C0::932e"];
    shouldBe("regex24.exec(input5);", "results");
    var input6 = "fed def";
    var results = ["def", "def"];
    shouldBe("regex24.exec(input6);", "results");
    var input7 = "Any old stuff";
    var results = ["ff", "ff"];
    shouldBe("regex24.exec(input7);", "results");
    // Failers
    var input8 = "0zzz";
    var results = null;
    shouldBe("regex24.exec(input8);", "results");
    var input9 = "gzzz";
    var results = null;
    shouldBe("regex24.exec(input9);", "results");
    var input10 = "fed\x20";
    var results = null;
    shouldBe("regex24.exec(input10);", "results");
    var input11 = "Any old rubbish";
    var results = null;
    shouldBe("regex24.exec(input11);", "results");

    var regex25 = /^.*\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;
    var input0 = ".1.2.3";
    var results = [".1.2.3", "1", "2", "3"];
    shouldBe("regex25.exec(input0);", "results");
    var input1 = "A.12.123.0";
    var results = ["A.12.123.0", "12", "123", "0"];
    shouldBe("regex25.exec(input1);", "results");
    // Failers
    var input2 = ".1.2.3333";
    var results = null;
    shouldBe("regex25.exec(input2);", "results");
    var input3 = "1.2.3";
    var results = null;
    shouldBe("regex25.exec(input3);", "results");
    var input4 = "1234.2.3";
    var results = null;
    shouldBe("regex25.exec(input4);", "results");

    var regex26 = /^(\d+)\s+IN\s+SOA\s+(\S+)\s+(\S+)\s*\(\s*$/;
    var input0 = "1 IN SOA non-sp1 non-sp2(";
    var results = ["1 IN SOA non-sp1 non-sp2(", "1", "non-sp1", "non-sp2"];
    shouldBe("regex26.exec(input0);", "results");
    var input1 = "1    IN    SOA    non-sp1    non-sp2   (";
    var results = ["1    IN    SOA    non-sp1    non-sp2   (", "1", "non-sp1", "non-sp2"];
    shouldBe("regex26.exec(input1);", "results");
    // Failers
    var input2 = "1IN SOA non-sp1 non-sp2(";
    var results = null;
    shouldBe("regex26.exec(input2);", "results");

    var regex27 = /^[a-zA-Z\d][a-zA-Z\d\-]*(\.[a-zA-Z\d][a-zA-z\d\-]*)*\.$/;
    var input0 = "a.";
    var results = ["a.", undefined];
    shouldBe("regex27.exec(input0);", "results");
    var input1 = "Z.";
    var results = ["Z.", undefined];
    shouldBe("regex27.exec(input1);", "results");
    var input2 = "2.";
    var results = ["2.", undefined];
    shouldBe("regex27.exec(input2);", "results");
    var input3 = "ab-c.pq-r.";
    var results = ["ab-c.pq-r.", ".pq-r"];
    shouldBe("regex27.exec(input3);", "results");
    var input4 = "sxk.zzz.ac.uk.";
    var results = ["sxk.zzz.ac.uk.", ".uk"];
    shouldBe("regex27.exec(input4);", "results");
    var input5 = "x-.y-.";
    var results = ["x-.y-.", ".y-"];
    shouldBe("regex27.exec(input5);", "results");
    // Failers
    var input6 = "-abc.peq.";
    var results = null;
    shouldBe("regex27.exec(input6);", "results");

    var regex28 = /^\*\.[a-z]([a-z\-\d]*[a-z\d]+)?(\.[a-z]([a-z\-\d]*[a-z\d]+)?)*$/;
    var input0 = "*.a";
    var results = ["*.a", undefined, undefined, undefined];
    shouldBe("regex28.exec(input0);", "results");
    var input1 = "*.b0-a";
    var results = ["*.b0-a", "0-a", undefined, undefined];
    shouldBe("regex28.exec(input1);", "results");
    var input2 = "*.c3-b.c";
    var results = ["*.c3-b.c", "3-b", ".c", undefined];
    shouldBe("regex28.exec(input2);", "results");
    var input3 = "*.c-a.b-c";
    var results = ["*.c-a.b-c", "-a", ".b-c", "-c"];
    shouldBe("regex28.exec(input3);", "results");
    // Failers
    var input4 = "*.0";
    var results = null;
    shouldBe("regex28.exec(input4);", "results");
    var input5 = "*.a-";
    var results = null;
    shouldBe("regex28.exec(input5);", "results");
    var input6 = "*.a-b.c-";
    var results = null;
    shouldBe("regex28.exec(input6);", "results");
    var input7 = "*.c-a.0-c";
    var results = null;
    shouldBe("regex28.exec(input7);", "results");

    var regex29 = /^(?=ab(de))(abd)(e)/;
    var input0 = "abde";
    var results = ["abde", "de", "abd", "e"];
    shouldBe("regex29.exec(input0);", "results");

    var regex30 = /^(?!(ab)de|x)(abd)(f)/;
    var input0 = "abdf";
    var results = ["abdf", undefined, "abd", "f"];
    shouldBe("regex30.exec(input0);", "results");

    var regex31 = /^(?=(ab(cd)))(ab)/;
    var input0 = "abcd";
    var results = ["ab", "abcd", "cd", "ab"];
    shouldBe("regex31.exec(input0);", "results");

    var regex32 = /^[\da-f](\.[\da-f])*$/i;
    var input0 = "a.b.c.d";
    var results = ["a.b.c.d", ".d"];
    shouldBe("regex32.exec(input0);", "results");
    var input1 = "A.B.C.D";
    var results = ["A.B.C.D", ".D"];
    shouldBe("regex32.exec(input1);", "results");
    var input2 = "a.b.c.1.2.3.C";
    var results = ["a.b.c.1.2.3.C", ".C"];
    shouldBe("regex32.exec(input2);", "results");

    var regex33 = /^\".*\"\s*(;.*)?$/;
    var input0 = '"1234"';
    var results = ['"1234"', undefined];
    shouldBe("regex33.exec(input0);", "results");
    var input1 = '"abcd" ;';
    var results = ['"abcd" ;', ";"];
    shouldBe("regex33.exec(input1);", "results");
    var input2 = '"" ; rhubarb';
    var results = ['"" ; rhubarb', "; rhubarb"];
    shouldBe("regex33.exec(input2);", "results");
    // Failers
    var input3 = '"1234" : things';
    var results = null;
    shouldBe("regex33.exec(input3);", "results");

    var regex34 = /^$/;
    var input0 = "";
    var results = [""];
    shouldBe("regex34.exec(input0);", "results");

    var regex35 = /^a\ b[c ]d$/;
    var input0 = "a bcd";
    var results = ["a bcd"];
    shouldBe("regex35.exec(input0);", "results");
    var input1 = "a b d";
    var results = ["a b d"];
    shouldBe("regex35.exec(input1);", "results");
    var input2 = "abcd";
    var results = null;
    shouldBe("regex35.exec(input2);", "results");
    var input3 = "ab d";
    var results = null;
    shouldBe("regex35.exec(input3);", "results");

    var regex36 = /^(a(b(c)))(d(e(f)))(h(i(j)))(k(l(m)))$/;
    var input0 = "abcdefhijklm";
    var results = ["abcdefhijklm", "abc", "bc", "c", "def", "ef", "f", "hij", "ij", "j", "klm", "lm", "m"];
    shouldBe("regex36.exec(input0);", "results");

    var regex37 = /^(?:a(b(c)))(?:d(e(f)))(?:h(i(j)))(?:k(l(m)))$/;
    var input0 = "abcdefhijklm";
    var results = ["abcdefhijklm", "bc", "c", "ef", "f", "ij", "j", "lm", "m"];
    shouldBe("regex37.exec(input0);", "results");

    var regex38 = /^[\w][\W][\s][\S][\d][\D][\n][\cc][\022]/;
    var input0 = "a+ Z0+\n\x03\x12";
    var results = ["a+ Z0+\x0a\x03\x12"];
    shouldBe("regex38.exec(input0);", "results");

    var regex39 = /^[.^$|()*+?{,}]+/;
    var input0 = ".^\$(*+)|{?,?}";
    var results = [".^$(*+)|{?,?}"];
    shouldBe("regex39.exec(input0);", "results");

    var regex40 = /^a*\w/;
    var input0 = "z";
    var results = ["z"];
    shouldBe("regex40.exec(input0);", "results");
    var input1 = "az";
    var results = ["az"];
    shouldBe("regex40.exec(input1);", "results");
    var input2 = "aaaz";
    var results = ["aaaz"];
    shouldBe("regex40.exec(input2);", "results");
    var input3 = "a";
    var results = ["a"];
    shouldBe("regex40.exec(input3);", "results");
    var input4 = "aa";
    var results = ["aa"];
    shouldBe("regex40.exec(input4);", "results");
    var input5 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex40.exec(input5);", "results");
    var input6 = "a+";
    var results = ["a"];
    shouldBe("regex40.exec(input6);", "results");
    var input7 = "aa+";
    var results = ["aa"];
    shouldBe("regex40.exec(input7);", "results");

    var regex41 = /^a*?\w/;
    var input0 = "z";
    var results = ["z"];
    shouldBe("regex41.exec(input0);", "results");
    var input1 = "az";
    var results = ["a"];
    shouldBe("regex41.exec(input1);", "results");
    var input2 = "aaaz";
    var results = ["a"];
    shouldBe("regex41.exec(input2);", "results");
    var input3 = "a";
    var results = ["a"];
    shouldBe("regex41.exec(input3);", "results");
    var input4 = "aa";
    var results = ["a"];
    shouldBe("regex41.exec(input4);", "results");
    var input5 = "aaaa";
    var results = ["a"];
    shouldBe("regex41.exec(input5);", "results");
    var input6 = "a+";
    var results = ["a"];
    shouldBe("regex41.exec(input6);", "results");
    var input7 = "aa+";
    var results = ["a"];
    shouldBe("regex41.exec(input7);", "results");

    var regex42 = /^a+\w/;
    var input0 = "az";
    var results = ["az"];
    shouldBe("regex42.exec(input0);", "results");
    var input1 = "aaaz";
    var results = ["aaaz"];
    shouldBe("regex42.exec(input1);", "results");
    var input2 = "aa";
    var results = ["aa"];
    shouldBe("regex42.exec(input2);", "results");
    var input3 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex42.exec(input3);", "results");
    var input4 = "aa+";
    var results = ["aa"];
    shouldBe("regex42.exec(input4);", "results");

    var regex43 = /^a+?\w/;
    var input0 = "az";
    var results = ["az"];
    shouldBe("regex43.exec(input0);", "results");
    var input1 = "aaaz";
    var results = ["aa"];
    shouldBe("regex43.exec(input1);", "results");
    var input2 = "aa";
    var results = ["aa"];
    shouldBe("regex43.exec(input2);", "results");
    var input3 = "aaaa";
    var results = ["aa"];
    shouldBe("regex43.exec(input3);", "results");
    var input4 = "aa+";
    var results = ["aa"];
    shouldBe("regex43.exec(input4);", "results");

    var regex44 = /^\d{8}\w{2,}/;
    var input0 = "1234567890";
    var results = ["1234567890"];
    shouldBe("regex44.exec(input0);", "results");
    var input1 = "12345678ab";
    var results = ["12345678ab"];
    shouldBe("regex44.exec(input1);", "results");
    var input2 = "12345678__";
    var results = ["12345678__"];
    shouldBe("regex44.exec(input2);", "results");
    // Failers
    var input3 = "1234567";
    var results = null;
    shouldBe("regex44.exec(input3);", "results");

    var regex45 = /^[aeiou\d]{4,5}$/;
    var input0 = "uoie";
    var results = ["uoie"];
    shouldBe("regex45.exec(input0);", "results");
    var input1 = "1234";
    var results = ["1234"];
    shouldBe("regex45.exec(input1);", "results");
    var input2 = "12345";
    var results = ["12345"];
    shouldBe("regex45.exec(input2);", "results");
    var input3 = "aaaaa";
    var results = ["aaaaa"];
    shouldBe("regex45.exec(input3);", "results");
    // Failers
    var input4 = "123456";
    var results = null;
    shouldBe("regex45.exec(input4);", "results");

    var regex46 = /^[aeiou\d]{4,5}?/;
    var input0 = "uoie";
    var results = ["uoie"];
    shouldBe("regex46.exec(input0);", "results");
    var input1 = "1234";
    var results = ["1234"];
    shouldBe("regex46.exec(input1);", "results");
    var input2 = "12345";
    var results = ["1234"];
    shouldBe("regex46.exec(input2);", "results");
    var input3 = "aaaaa";
    var results = ["aaaa"];
    shouldBe("regex46.exec(input3);", "results");
    var input4 = "123456";
    var results = ["1234"];
    shouldBe("regex46.exec(input4);", "results");

    var regex47 = /^(abc|def)=(\1){2,3}$/;
    var input0 = "abc=abcabc";
    var results = ["abc=abcabc", "abc", "abc"];
    shouldBe("regex47.exec(input0);", "results");
    var input1 = "def=defdefdef";
    var results = ["def=defdefdef", "def", "def"];
    shouldBe("regex47.exec(input1);", "results");
    // Failers
    var input2 = "abc=defdef";
    var results = null;
    shouldBe("regex47.exec(input2);", "results");

    var regex48 = /^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)\11*(\3\4)\1(?:)2$/;
    var input0 = "abcdefghijkcda2";
    var results = ["abcdefghijkcda2", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "cd"];
    shouldBe("regex48.exec(input0);", "results");
    var input1 = "abcdefghijkkkkcda2";
    var results = ["abcdefghijkkkkcda2", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "cd"];
    shouldBe("regex48.exec(input1);", "results");

    var regex49 = /(cat(a(ract|tonic)|erpillar)) \1()2(3)/;
    var input0 = "cataract cataract23";
    var results = ["cataract cataract23", "cataract", "aract", "ract", "", "3"];
    shouldBe("regex49.exec(input0);", "results");
    var input1 = "catatonic catatonic23";
    var results = ["catatonic catatonic23", "catatonic", "atonic", "tonic", "", "3"];
    shouldBe("regex49.exec(input1);", "results");
    var input2 = "caterpillar caterpillar23";
    var results = ["caterpillar caterpillar23", "caterpillar", "erpillar", undefined, "", "3"];
    shouldBe("regex49.exec(input2);", "results");

    var regex50 =
        /^From +([^ ]+) +[a-zA-Z][a-zA-Z][a-zA-Z] +[a-zA-Z][a-zA-Z][a-zA-Z] +[0-9]?[0-9] +[0-9][0-9]:[0-9][0-9]/;
    var input0 = "From abcd  Mon Sep 01 12:33:02 1997";
    var results = ["From abcd  Mon Sep 01 12:33", "abcd"];
    shouldBe("regex50.exec(input0);", "results");

    var regex51 = /^From\s+\S+\s+([a-zA-Z]{3}\s+){2}\d{1,2}\s+\d\d:\d\d/;
    var input0 = "From abcd  Mon Sep 01 12:33:02 1997";
    var results = ["From abcd  Mon Sep 01 12:33", "Sep "];
    shouldBe("regex51.exec(input0);", "results");
    var input1 = "From abcd  Mon Sep  1 12:33:02 1997";
    var results = ["From abcd  Mon Sep  1 12:33", "Sep  "];
    shouldBe("regex51.exec(input1);", "results");
    // Failers
    var input2 = "From abcd  Sep 01 12:33:02 1997";
    var results = null;
    shouldBe("regex51.exec(input2);", "results");

    var regex52 = /^12.34/;
    // Failers
    var input0 = "12\n34";
    var results = null;
    shouldBe("regex52.exec(input0);", "results");
    var input1 = "12\r34";
    var results = null;
    shouldBe("regex52.exec(input1);", "results");

    var regex53 = /\w+(?=\t)/;
    var input0 = "the quick brown\t fox";
    var results = ["brown"];
    shouldBe("regex53.exec(input0);", "results");

    var regex54 = /foo(?!bar)(.*)/;
    var input0 = "foobar is foolish see?";
    var results = ["foolish see?", "lish see?"];
    shouldBe("regex54.exec(input0);", "results");

    var regex55 = /(?:(?!foo)...|^.{0,2})bar(.*)/;
    var input0 = "foobar crowbar etc";
    var results = ["rowbar etc", " etc"];
    shouldBe("regex55.exec(input0);", "results");
    var input1 = "barrel";
    var results = ["barrel", "rel"];
    shouldBe("regex55.exec(input1);", "results");
    var input2 = "2barrel";
    var results = ["2barrel", "rel"];
    shouldBe("regex55.exec(input2);", "results");
    var input3 = "A barrel";
    var results = ["A barrel", "rel"];
    shouldBe("regex55.exec(input3);", "results");

    var regex56 = /^(\D*)(?=\d)(?!123)/;
    var input0 = "abc456";
    var results = ["abc", "abc"];
    shouldBe("regex56.exec(input0);", "results");
    // Failers
    var input1 = "abc123";
    var results = null;
    shouldBe("regex56.exec(input1);", "results");

    var regex57 = /^(a)\1{2,3}(.)/;
    var input0 = "aaab";
    var results = ["aaab", "a", "b"];
    shouldBe("regex57.exec(input0);", "results");
    var input1 = "aaaab";
    var results = ["aaaab", "a", "b"];
    shouldBe("regex57.exec(input1);", "results");
    var input2 = "aaaaab";
    var results = ["aaaaa", "a", "a"];
    shouldBe("regex57.exec(input2);", "results");
    var input3 = "aaaaaab";
    var results = ["aaaaa", "a", "a"];
    shouldBe("regex57.exec(input3);", "results");

    var regex58 = /(?!^)abc/;
    var input0 = "the abc";
    var results = ["abc"];
    shouldBe("regex58.exec(input0);", "results");
    // Failers
    var input1 = "abc";
    var results = null;
    shouldBe("regex58.exec(input1);", "results");

    var regex59 = /(?=^)abc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex59.exec(input0);", "results");
    // Failers
    var input1 = "the abc";
    var results = null;
    shouldBe("regex59.exec(input1);", "results");

    var regex60 = /^[ab]{1,3}(ab*|b)/;
    var input0 = "aabbbbb";
    var results = ["aabb", "b"];
    shouldBe("regex60.exec(input0);", "results");

    var regex61 = /^[ab]{1,3}?(ab*|b)/;
    var input0 = "aabbbbb";
    var results = ["aabbbbb", "abbbbb"];
    shouldBe("regex61.exec(input0);", "results");

    var regex62 = /^[ab]{1,3}?(ab*?|b)/;
    var input0 = "aabbbbb";
    var results = ["aa", "a"];
    shouldBe("regex62.exec(input0);", "results");

    var regex63 = /^[ab]{1,3}(ab*?|b)/;
    var input0 = "aabbbbb";
    var results = ["aabb", "b"];
    shouldBe("regex63.exec(input0);", "results");

    var regex64 = /abc\0def\00pqr\000xyz\0000AB/;
    var input0 = "abc\0def\0pqr\0xyz\0" + "0AB";
    var results = ["abc\0def\0pqr\0xyz\0" + "0AB"];
    shouldBe("regex64.exec(input0);", "results");
    var input1 = "abc456 abc\0def\0pqr\0xyz\0" + "0ABCDE";
    var results = ["abc\0def\0pqr\0xyz\0" + "0AB"];
    shouldBe("regex64.exec(input1);", "results");

    var regex65 = /abc\x0def\x00pqr\x000xyz\x0000AB/;
    var input0 = "abc\x0def\x00pqr\x000xyz\x0000AB";
    var results = ["abc\x0def\x00pqr\x000xyz\x0000AB"];
    shouldBe("regex65.exec(input0);", "results");
    var input1 = "abc456 abc\x0def\x00pqr\x000xyz\x0000ABCDE";
    var results = ["abc\x0def\x00pqr\x000xyz\x0000AB"];
    shouldBe("regex65.exec(input1);", "results");

    var regex66 = /^[\000-\037]/;
    var input0 = "\0A";
    var results = ["\x00"];
    shouldBe("regex66.exec(input0);", "results");
    var input1 = "\01B";
    var results = ["\x01"];
    shouldBe("regex66.exec(input1);", "results");
    var input2 = "\037C";
    var results = ["\x1f"];
    shouldBe("regex66.exec(input2);", "results");

    var regex67 = /\0*/;
    var input0 = "\0\0\0\0";
    var results = ["\x00\x00\x00\x00"];
    shouldBe("regex67.exec(input0);", "results");

    var regex68 = /A\0{2,3}Z/;
    var input0 = "The A\0\0Z";
    var results = ["A\x00\x00Z"];
    shouldBe("regex68.exec(input0);", "results");
    var input1 = "An A\0\0\0Z";
    var results = ["A\x00\x00\x00Z"];
    shouldBe("regex68.exec(input1);", "results");
    // Failers
    var input2 = "A\0Z";
    var results = null;
    shouldBe("regex68.exec(input2);", "results");
    var input3 = "A\0\0\0\0Z";
    var results = null;
    shouldBe("regex68.exec(input3);", "results");

    var regex69 = /^(cow|)\1(bell)/;
    var input0 = "cowcowbell";
    var results = ["cowcowbell", "cow", "bell"];
    shouldBe("regex69.exec(input0);", "results");
    var input1 = "bell";
    var results = ["bell", "", "bell"];
    shouldBe("regex69.exec(input1);", "results");
    // Failers
    var input2 = "cowbell";
    var results = null;
    shouldBe("regex69.exec(input2);", "results");

    var regex70 = /^\s/;
    var input0 = "\040abc";
    var results = [" "];
    shouldBe("regex70.exec(input0);", "results");
    var input1 = "\x0cabc";
    var results = ["\x0c"];
    shouldBe("regex70.exec(input1);", "results");
    var input2 = "\nabc";
    var results = ["\x0a"];
    shouldBe("regex70.exec(input2);", "results");
    var input3 = "\rabc";
    var results = ["\x0d"];
    shouldBe("regex70.exec(input3);", "results");
    var input4 = "\tabc";
    var results = ["\x09"];
    shouldBe("regex70.exec(input4);", "results");
    // Failers
    var input5 = "abc";
    var results = null;
    shouldBe("regex70.exec(input5);", "results");

    var regex71 = /^(a|)\1*b/;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex71.exec(input0);", "results");
    var input1 = "aaaab";
    var results = ["aaaab", "a"];
    shouldBe("regex71.exec(input1);", "results");
    var input2 = "b";
    var results = ["b", ""];
    shouldBe("regex71.exec(input2);", "results");
    // Failers
    var input3 = "acb";
    var results = null;
    shouldBe("regex71.exec(input3);", "results");

    var regex72 = /^(a|)\1+b/;
    var input0 = "aab";
    var results = ["aab", "a"];
    shouldBe("regex72.exec(input0);", "results");
    var input1 = "aaaab";
    var results = ["aaaab", "a"];
    shouldBe("regex72.exec(input1);", "results");
    var input2 = "b";
    var results = ["b", ""];
    shouldBe("regex72.exec(input2);", "results");
    // Failers
    var input3 = "ab";
    var results = null;
    shouldBe("regex72.exec(input3);", "results");

    var regex73 = /^(a|)\1?b/;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex73.exec(input0);", "results");
    var input1 = "aab";
    var results = ["aab", "a"];
    shouldBe("regex73.exec(input1);", "results");
    var input2 = "b";
    var results = ["b", ""];
    shouldBe("regex73.exec(input2);", "results");
    // Failers
    var input3 = "acb";
    var results = null;
    shouldBe("regex73.exec(input3);", "results");

    var regex74 = /^(a|)\1{2}b/;
    var input0 = "aaab";
    var results = ["aaab", "a"];
    shouldBe("regex74.exec(input0);", "results");
    var input1 = "b";
    var results = ["b", ""];
    shouldBe("regex74.exec(input1);", "results");
    // Failers
    var input2 = "ab";
    var results = null;
    shouldBe("regex74.exec(input2);", "results");
    var input3 = "aab";
    var results = null;
    shouldBe("regex74.exec(input3);", "results");
    var input4 = "aaaab";
    var results = null;
    shouldBe("regex74.exec(input4);", "results");

    var regex75 = /^(a|)\1{2,3}b/;
    var input0 = "aaab";
    var results = ["aaab", "a"];
    shouldBe("regex75.exec(input0);", "results");
    var input1 = "aaaab";
    var results = ["aaaab", "a"];
    shouldBe("regex75.exec(input1);", "results");
    var input2 = "b";
    var results = ["b", ""];
    shouldBe("regex75.exec(input2);", "results");
    // Failers
    var input3 = "ab";
    var results = null;
    shouldBe("regex75.exec(input3);", "results");
    var input4 = "aab";
    var results = null;
    shouldBe("regex75.exec(input4);", "results");
    var input5 = "aaaaab";
    var results = null;
    shouldBe("regex75.exec(input5);", "results");

    var regex76 = /ab{1,3}bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex76.exec(input0);", "results");
    var input1 = "abbbc";
    var results = ["abbbc"];
    shouldBe("regex76.exec(input1);", "results");
    var input2 = "abbc";
    var results = ["abbc"];
    shouldBe("regex76.exec(input2);", "results");
    // Failers
    var input3 = "abc";
    var results = null;
    shouldBe("regex76.exec(input3);", "results");
    var input4 = "abbbbbc";
    var results = null;
    shouldBe("regex76.exec(input4);", "results");

    var regex77 = /([^.]*)\.([^:]*):[T ]+(.*)/;
    var input0 = "track1.title:TBlah blah blah";
    var results = ["track1.title:TBlah blah blah", "track1", "title", "Blah blah blah"];
    shouldBe("regex77.exec(input0);", "results");

    var regex78 = /([^.]*)\.([^:]*):[T ]+(.*)/i;
    var input0 = "track1.title:TBlah blah blah";
    var results = ["track1.title:TBlah blah blah", "track1", "title", "Blah blah blah"];
    shouldBe("regex78.exec(input0);", "results");

    var regex79 = /([^.]*)\.([^:]*):[t ]+(.*)/i;
    var input0 = "track1.title:TBlah blah blah";
    var results = ["track1.title:TBlah blah blah", "track1", "title", "Blah blah blah"];
    shouldBe("regex79.exec(input0);", "results");

    var regex80 = /^[W-c]+$/;
    var input0 = "WXY_^abc";
    var results = ["WXY_^abc"];
    shouldBe("regex80.exec(input0);", "results");
    // Failers
    var input1 = "wxy";
    var results = null;
    shouldBe("regex80.exec(input1);", "results");

    var regex81 = /^[W-c]+$/i;
    var input0 = "WXY_^abc";
    var results = ["WXY_^abc"];
    shouldBe("regex81.exec(input0);", "results");
    var input1 = "wxy_^ABC";
    var results = ["wxy_^ABC"];
    shouldBe("regex81.exec(input1);", "results");

    var regex82 = /^[\x3f-\x5F]+$/i;
    var input0 = "WXY_^abc";
    var results = ["WXY_^abc"];
    shouldBe("regex82.exec(input0);", "results");
    var input1 = "wxy_^ABC";
    var results = ["wxy_^ABC"];
    shouldBe("regex82.exec(input1);", "results");

    var regex83 = /^abc$/m;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex83.exec(input0);", "results");
    var input1 = "qqq\nabc";
    var results = ["abc"];
    shouldBe("regex83.exec(input1);", "results");
    var input2 = "abc\nzzz";
    var results = ["abc"];
    shouldBe("regex83.exec(input2);", "results");
    var input3 = "qqq\nabc\nzzz";
    var results = ["abc"];
    shouldBe("regex83.exec(input3);", "results");

    var regex84 = /^abc$/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex84.exec(input0);", "results");
    var input1 = "abbbbc";
    var results = null;
    shouldBe("regex84.exec(input1);", "results");
    var input2 = "abcc";
    var results = null;
    shouldBe("regex84.exec(input2);", "results");
    var input3 = "qqq\nabc";
    var results = null;
    shouldBe("regex84.exec(input3);", "results");
    var input4 = "abc\nzzz";
    var results = null;
    shouldBe("regex84.exec(input4);", "results");
    var input5 = "qqq\nabc\nzzz";
    var results = null;
    shouldBe("regex84.exec(input5);", "results");

    var regex85 = /^abc$/m;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex85.exec(input0);", "results");
    var input1 = "abc\n";
    var results = ["abc"];
    shouldBe("regex85.exec(input1);", "results");
    var input2 = "qqq\nabc";
    var results = ["abc"];
    shouldBe("regex85.exec(input2);", "results");
    var input3 = "abc\nzzz";
    var results = ["abc"];
    shouldBe("regex85.exec(input3);", "results");
    var input4 = "qqq\nabc\nzzz";
    var results = ["abc"];
    shouldBe("regex85.exec(input4);", "results");

    var regex86 = /^([\w\W])*$/;
    var input0 = "abc\ndef";
    var results = ["abc\x0adef", "f"];
    shouldBe("regex86.exec(input0);", "results");

    var regex87 = /^(.)*$/m;
    var input0 = "abc\ndef";
    var results = ["abc", "c"];
    shouldBe("regex87.exec(input0);", "results");

    var regex88 = /(?:b)|(?::+)/;
    var input0 = "b::c";
    var results = ["b"];
    shouldBe("regex88.exec(input0);", "results");
    var input1 = "c::b";
    var results = ["::"];
    shouldBe("regex88.exec(input1);", "results");

    var regex89 = /[-az]+/;
    var input0 = "az-";
    var results = ["az-"];
    shouldBe("regex89.exec(input0);", "results");
    // Failers
    var input1 = "b";
    var results = null;
    shouldBe("regex89.exec(input1);", "results");

    var regex90 = /[az-]+/;
    var input0 = "za-";
    var results = ["za-"];
    shouldBe("regex90.exec(input0);", "results");
    // Failers
    var input1 = "b";
    var results = null;
    shouldBe("regex90.exec(input1);", "results");

    var regex91 = /[a\-z]+/;
    var input0 = "a-z";
    var results = ["a-z"];
    shouldBe("regex91.exec(input0);", "results");
    // Failers
    var input1 = "b";
    var results = null;
    shouldBe("regex91.exec(input1);", "results");

    var regex92 = /[a-z]+/;
    var input0 = "abcdxyz";
    var results = ["abcdxyz"];
    shouldBe("regex92.exec(input0);", "results");

    var regex93 = /[\d-]+/;
    var input0 = "12-34";
    var results = ["12-34"];
    shouldBe("regex93.exec(input0);", "results");
    // Failers
    var input1 = "aaa";
    var results = null;
    shouldBe("regex93.exec(input1);", "results");

    var regex94 = /[\d-z]+/;
    var input0 = "12-34z";
    var results = ["12-34z"];
    shouldBe("regex94.exec(input0);", "results");
    // Failers
    var input1 = "aaa";
    var results = null;
    shouldBe("regex94.exec(input1);", "results");

    var regex95 = /\x5c/;
    var input0 = "\\";
    var results = ["\\"];
    shouldBe("regex95.exec(input0);", "results");

    var regex96 = /\x20Z/;
    var input0 = "the Zoo";
    var results = [" Z"];
    shouldBe("regex96.exec(input0);", "results");
    // Failers
    var input1 = "Zulu";
    var results = null;
    shouldBe("regex96.exec(input1);", "results");

    var regex97 = /(abc)\1/i;
    var input0 = "abcabc";
    var results = ["abcabc", "abc"];
    shouldBe("regex97.exec(input0);", "results");
    var input1 = "ABCabc";
    var results = ["ABCabc", "ABC"];
    shouldBe("regex97.exec(input1);", "results");
    var input2 = "abcABC";
    var results = ["abcABC", "abc"];
    shouldBe("regex97.exec(input2);", "results");

    var regex98 = /ab{3cd/;
    var input0 = "ab{3cd";
    var results = ["ab{3cd"];
    shouldBe("regex98.exec(input0);", "results");

    var regex99 = /ab{3,cd/;
    var input0 = "ab{3,cd";
    var results = ["ab{3,cd"];
    shouldBe("regex99.exec(input0);", "results");

    var regex100 = /ab{3,4a}cd/;
    var input0 = "ab{3,4a}cd";
    var results = ["ab{3,4a}cd"];
    shouldBe("regex100.exec(input0);", "results");

    var regex101 = /{4,5a}bc/;
    var input0 = "{4,5a}bc";
    var results = ["{4,5a}bc"];
    shouldBe("regex101.exec(input0);", "results");

    var regex102 = /abc$/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex102.exec(input0);", "results");
    // Failers
    var input1 = "abc\n";
    var results = null;
    shouldBe("regex102.exec(input1);", "results");
    var input2 = "abc\ndef";
    var results = null;
    shouldBe("regex102.exec(input2);", "results");

    var regex103 = /abc$/m;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex103.exec(input0);", "results");
    var input1 = "abc\n";
    var results = ["abc"];
    shouldBe("regex103.exec(input1);", "results");
    var input2 = "abc\ndef";
    var results = ["abc"];
    shouldBe("regex103.exec(input2);", "results");

    var regex104 = /(abc)\123/;
    var input0 = "abc\x53";
    var results = ["abcS", "abc"];
    shouldBe("regex104.exec(input0);", "results");

    var regex105 = /(abc)\223/;
    var input0 = "abc\x93";
    var results = ["abc\x93", "abc"];
    shouldBe("regex105.exec(input0);", "results");

    var regex106 = /(abc)\323/;
    var input0 = "abc\xd3";
    var results = ["abc\xd3", "abc"];
    shouldBe("regex106.exec(input0);", "results");

    var regex107 = /(abc)\100/;
    var input0 = "abc\x40";
    var results = ["abc@", "abc"];
    shouldBe("regex107.exec(input0);", "results");
    var input1 = "abc\100";
    var results = ["abc@", "abc"];
    shouldBe("regex107.exec(input1);", "results");

    var regex108 = /(abc)\1000/;
    var input0 = "abc\x400";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input0);", "results");
    var input1 = "abc\x40\x30";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input1);", "results");
    var input2 = "abc\1000";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input2);", "results");
    var input3 = "abc\100\x30";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input3);", "results");
    var input4 = "abc\100\060";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input4);", "results");
    var input5 = "abc\100\60";
    var results = ["abc@0", "abc"];
    shouldBe("regex108.exec(input5);", "results");

    var regex109 = /abc\081/;
    var input0 = "abc\081";
    var results = ["abc\x0081"];
    shouldBe("regex109.exec(input0);", "results");
    var input1 = "abc\0\x38\x31";
    var results = ["abc\x0081"];
    shouldBe("regex109.exec(input1);", "results");

    var regex110 = /(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)\12\123/;
    var input0 = "abcdefghijkllS";
    var results = ["abcdefghijkllS", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"];
    shouldBe("regex110.exec(input0);", "results");

    var regex111 = /(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)\12\123/;
    var input0 = "abcdefghijk\12S";
    var results = ["abcdefghijk\x0aS", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"];
    shouldBe("regex111.exec(input0);", "results");

    var regex112 = /ab\idef/;
    var input0 = "abidef";
    var results = ["abidef"];
    shouldBe("regex112.exec(input0);", "results");

    var regex113 = /a{0}bc/;
    var input0 = "bc";
    var results = ["bc"];
    shouldBe("regex113.exec(input0);", "results");

    var regex114 = /(?:a|(?:bc)){0,0}?xyz/;
    var input0 = "xyz";
    var results = ["xyz"];
    shouldBe("regex114.exec(input0);", "results");

    var regex115 = /abc[\10]de/;
    var input0 = "abc\010de";
    var results = ["abc\x08de"];
    shouldBe("regex115.exec(input0);", "results");

    var regex116 = /abc[\1]de/;
    var input0 = "abc\1de";
    var results = ["abc\x01de"];
    shouldBe("regex116.exec(input0);", "results");

    var regex117 = /(abc)[\1]de/;
    var input0 = "abc\1de";
    var results = ["abc\x01de", "abc"];
    shouldBe("regex117.exec(input0);", "results");

    var regex118 = /a.b/;
    var input0 = "a\nb";
    var results = null;
    shouldBe("regex118.exec(input0);", "results");

    var regex119 = /^([^a])([^\b])([^c]*)([^d]{3,4})/;
    var input0 = "baNOTccccd";
    var results = ["baNOTcccc", "b", "a", "NOT", "cccc"];
    shouldBe("regex119.exec(input0);", "results");
    var input1 = "baNOTcccd";
    var results = ["baNOTccc", "b", "a", "NOT", "ccc"];
    shouldBe("regex119.exec(input1);", "results");
    var input2 = "baNOTccd";
    var results = ["baNOTcc", "b", "a", "NO", "Tcc"];
    shouldBe("regex119.exec(input2);", "results");
    var input3 = "bacccd";
    var results = ["baccc", "b", "a", "", "ccc"];
    shouldBe("regex119.exec(input3);", "results");
    // Failers
    var input4 = "anything";
    var results = null;
    shouldBe("regex119.exec(input4);", "results");
    var input5 = "b\bc";
    var results = null;
    shouldBe("regex119.exec(input5);", "results");
    var input6 = "baccd";
    var results = null;
    shouldBe("regex119.exec(input6);", "results");

    var regex120 = /[^a]/;
    var input0 = "Abc";
    var results = ["A"];
    shouldBe("regex120.exec(input0);", "results");

    var regex121 = /[^a]/i;
    var input0 = "Abc";
    var results = ["b"];
    shouldBe("regex121.exec(input0);", "results");

    var regex122 = /[^a]+/;
    var input0 = "AAAaAbc";
    var results = ["AAA"];
    shouldBe("regex122.exec(input0);", "results");

    var regex123 = /[^a]+/i;
    var input0 = "AAAaAbc";
    var results = ["bc"];
    shouldBe("regex123.exec(input0);", "results");

    var regex124 = /[^a]+/;
    var input0 = "bbb\nccc";
    var results = ["bbb\x0accc"];
    shouldBe("regex124.exec(input0);", "results");

    var regex125 = /[^k]$/;
    var input0 = "abc";
    var results = ["c"];
    shouldBe("regex125.exec(input0);", "results");
    // Failers
    var input1 = "abk";
    var results = null;
    shouldBe("regex125.exec(input1);", "results");

    var regex126 = /[^k]{2,3}$/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex126.exec(input0);", "results");
    var input1 = "kbc";
    var results = ["bc"];
    shouldBe("regex126.exec(input1);", "results");
    var input2 = "kabc";
    var results = ["abc"];
    shouldBe("regex126.exec(input2);", "results");
    // Failers
    var input3 = "abk";
    var results = null;
    shouldBe("regex126.exec(input3);", "results");
    var input4 = "akb";
    var results = null;
    shouldBe("regex126.exec(input4);", "results");
    var input5 = "akk";
    var results = null;
    shouldBe("regex126.exec(input5);", "results");

    var regex127 = /^\d{8,}\@.+[^k]$/;
    var input0 = "12345678\@a.b.c.d";
    var results = ["12345678@a.b.c.d"];
    shouldBe("regex127.exec(input0);", "results");
    var input1 = "123456789\@x.y.z";
    var results = ["123456789@x.y.z"];
    shouldBe("regex127.exec(input1);", "results");
    // Failers
    var input2 = "12345678\@x.y.uk";
    var results = null;
    shouldBe("regex127.exec(input2);", "results");
    var input3 = "1234567\@a.b.c.d";
    var results = null;
    shouldBe("regex127.exec(input3);", "results");

    var regex128 = /(a)\1{8,}/;
    var input0 = "aaaaaaaaa";
    var results = ["aaaaaaaaa", "a"];
    shouldBe("regex128.exec(input0);", "results");
    var input1 = "aaaaaaaaaa";
    var results = ["aaaaaaaaaa", "a"];
    shouldBe("regex128.exec(input1);", "results");
    // Failers
    var input2 = "aaaaaaa";
    var results = null;
    shouldBe("regex128.exec(input2);", "results");

    var regex129 = /[^a]/;
    var input0 = "aaaabcd";
    var results = ["b"];
    shouldBe("regex129.exec(input0);", "results");
    var input1 = "aaAabcd";
    var results = ["A"];
    shouldBe("regex129.exec(input1);", "results");

    var regex130 = /[^a]/i;
    var input0 = "aaaabcd";
    var results = ["b"];
    shouldBe("regex130.exec(input0);", "results");
    var input1 = "aaAabcd";
    var results = ["b"];
    shouldBe("regex130.exec(input1);", "results");

    var regex131 = /[^az]/;
    var input0 = "aaaabcd";
    var results = ["b"];
    shouldBe("regex131.exec(input0);", "results");
    var input1 = "aaAabcd";
    var results = ["A"];
    shouldBe("regex131.exec(input1);", "results");

    var regex132 = /[^az]/i;
    var input0 = "aaaabcd";
    var results = ["b"];
    shouldBe("regex132.exec(input0);", "results");
    var input1 = "aaAabcd";
    var results = ["b"];
    shouldBe("regex132.exec(input1);", "results");

    var regex133 =
        /\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037\040\041\042\043\044\045\046\047\050\051\052\053\054\055\056\057\060\061\062\063\064\065\066\067\070\071\072\073\074\075\076\077\100\101\102\103\104\105\106\107\110\111\112\113\114\115\116\117\120\121\122\123\124\125\126\127\130\131\132\133\134\135\136\137\140\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157\160\161\162\163\164\165\166\167\170\171\172\173\174\175\176\177\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377/;
    var input0 =
        "\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037\040\041\042\043\044\045\046\047\050\051\052\053\054\055\056\057\060\061\062\063\064\065\066\067\070\071\072\073\074\075\076\077\100\101\102\103\104\105\106\107\110\111\112\113\114\115\116\117\120\121\122\123\124\125\126\127\130\131\132\133\134\135\136\137\140\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157\160\161\162\163\164\165\166\167\170\171\172\173\174\175\176\177\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377";
    var results = [
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !\"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\x7f\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff",
    ];
    shouldBe("regex133.exec(input0);", "results");

    var regex134 = /P[^*]TAIRE[^*]{1,6}?LL/;
    var input0 = "xxxxxxxxxxxPSTAIREISLLxxxxxxxxx";
    var results = ["PSTAIREISLL"];
    shouldBe("regex134.exec(input0);", "results");

    var regex135 = /P[^*]TAIRE[^*]{1,}?LL/;
    var input0 = "xxxxxxxxxxxPSTAIREISLLxxxxxxxxx";
    var results = ["PSTAIREISLL"];
    shouldBe("regex135.exec(input0);", "results");

    var regex136 = /(\.\d\d[1-9]?)\d+/;
    var input0 = "1.230003938";
    var results = [".230003938", ".23"];
    shouldBe("regex136.exec(input0);", "results");
    var input1 = "1.875000282";
    var results = [".875000282", ".875"];
    shouldBe("regex136.exec(input1);", "results");
    var input2 = "1.235";
    var results = [".235", ".23"];
    shouldBe("regex136.exec(input2);", "results");

    var regex137 = /(\.\d\d((?=0)|\d(?=\d)))/;
    var input0 = "1.230003938";
    var results = [".23", ".23", ""];
    shouldBe("regex137.exec(input0);", "results");
    var input1 = "1.875000282";
    var results = [".875", ".875", "5"];
    shouldBe("regex137.exec(input1);", "results");
    // Failers
    var input2 = "1.235";
    var results = null;
    shouldBe("regex137.exec(input2);", "results");

    var regex138 = /a(?:)b/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex138.exec(input0);", "results");

    var regex139 = /\b(foo)\s+(\w+)/i;
    var input0 = "Food is on the foo table";
    var results = ["foo table", "foo", "table"];
    shouldBe("regex139.exec(input0);", "results");

    var regex140 = /foo(.*)bar/;
    var input0 = "The food is under the bar in the barn.";
    var results = ["food is under the bar in the bar", "d is under the bar in the "];
    shouldBe("regex140.exec(input0);", "results");

    var regex141 = /foo(.*?)bar/;
    var input0 = "The food is under the bar in the barn.";
    var results = ["food is under the bar", "d is under the "];
    shouldBe("regex141.exec(input0);", "results");

    var regex142 = /(.*)(\d*)/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: 53147", ""];
    shouldBe("regex142.exec(input0);", "results");

    var regex143 = /(.*)(\d+)/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: 5314", "7"];
    shouldBe("regex143.exec(input0);", "results");

    var regex144 = /(.*?)(\d*)/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["", "", ""];
    shouldBe("regex144.exec(input0);", "results");

    var regex145 = /(.*?)(\d+)/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2", "I have ", "2"];
    shouldBe("regex145.exec(input0);", "results");

    var regex146 = /(.*)(\d+)$/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: 5314", "7"];
    shouldBe("regex146.exec(input0);", "results");

    var regex147 = /(.*?)(\d+)$/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: ", "53147"];
    shouldBe("regex147.exec(input0);", "results");

    var regex148 = /(.*)\b(\d+)$/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: ", "53147"];
    shouldBe("regex148.exec(input0);", "results");

    var regex149 = /(.*\D)(\d+)$/;
    var input0 = "I have 2 numbers: 53147";
    var results = ["I have 2 numbers: 53147", "I have 2 numbers: ", "53147"];
    shouldBe("regex149.exec(input0);", "results");

    var regex150 = /^\D*(?!123)/;
    var input0 = "ABC123";
    var results = ["AB"];
    shouldBe("regex150.exec(input0);", "results");

    var regex151 = /^(\D*)(?=\d)(?!123)/;
    var input0 = "ABC445";
    var results = ["ABC", "ABC"];
    shouldBe("regex151.exec(input0);", "results");
    // Failers
    var input1 = "ABC123";
    var results = null;
    shouldBe("regex151.exec(input1);", "results");

    var regex152 = /^[W-]46]/;
    var input0 = "W46]789";
    var results = ["W46]"];
    shouldBe("regex152.exec(input0);", "results");
    var input1 = "-46]789";
    var results = ["-46]"];
    shouldBe("regex152.exec(input1);", "results");
    // Failers
    var input2 = "Wall";
    var results = null;
    shouldBe("regex152.exec(input2);", "results");
    var input3 = "Zebra";
    var results = null;
    shouldBe("regex152.exec(input3);", "results");
    var input4 = "42";
    var results = null;
    shouldBe("regex152.exec(input4);", "results");
    var input5 = "[abcd]";
    var results = null;
    shouldBe("regex152.exec(input5);", "results");
    var input6 = "]abcd[";
    var results = null;
    shouldBe("regex152.exec(input6);", "results");

    var regex153 = /^[W-\]46]/;
    var input0 = "W46]789";
    var results = ["W"];
    shouldBe("regex153.exec(input0);", "results");
    var input1 = "Wall";
    var results = ["W"];
    shouldBe("regex153.exec(input1);", "results");
    var input2 = "Zebra";
    var results = ["Z"];
    shouldBe("regex153.exec(input2);", "results");
    var input3 = "Xylophone";
    var results = ["X"];
    shouldBe("regex153.exec(input3);", "results");
    var input4 = "42";
    var results = ["4"];
    shouldBe("regex153.exec(input4);", "results");
    var input5 = "[abcd]";
    var results = ["["];
    shouldBe("regex153.exec(input5);", "results");
    var input6 = "]abcd[";
    var results = ["]"];
    shouldBe("regex153.exec(input6);", "results");
    var input7 = "\\backslash";
    var results = ["\\"];
    shouldBe("regex153.exec(input7);", "results");
    // Failers
    var input8 = "-46]789";
    var results = null;
    shouldBe("regex153.exec(input8);", "results");
    var input9 = "well";
    var results = null;
    shouldBe("regex153.exec(input9);", "results");

    var regex154 = /\d\d\/\d\d\/\d\d\d\d/;
    var input0 = "01/01/2000";
    var results = ["01/01/2000"];
    shouldBe("regex154.exec(input0);", "results");

    var regex155 = /word (?:[a-zA-Z0-9]+ ){0,10}otherword/;
    var input0 = "word cat dog elephant mussel cow horse canary baboon snake shark otherword";
    var results = ["word cat dog elephant mussel cow horse canary baboon snake shark otherword"];
    shouldBe("regex155.exec(input0);", "results");
    var input1 = "word cat dog elephant mussel cow horse canary baboon snake shark";
    var results = null;
    shouldBe("regex155.exec(input1);", "results");

    var regex156 = /word (?:[a-zA-Z0-9]+ ){0,300}otherword/;
    var input0 =
        "word cat dog elephant mussel cow horse canary baboon snake shark the quick brown fox and the lazy dog and several other words getting close to thirty by now I hope";
    var results = null;
    shouldBe("regex156.exec(input0);", "results");

    var regex157 = /^(?:a){0,0}/;
    var input0 = "bcd";
    var results = [""];
    shouldBe("regex157.exec(input0);", "results");
    var input1 = "abc";
    var results = [""];
    shouldBe("regex157.exec(input1);", "results");
    var input2 = "aab";
    var results = [""];
    shouldBe("regex157.exec(input2);", "results");

    var regex158 = /^(a){0,1}/;
    var input0 = "bcd";
    var results = ["", undefined];
    shouldBe("regex158.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex158.exec(input1);", "results");
    var input2 = "aab";
    var results = ["a", "a"];
    shouldBe("regex158.exec(input2);", "results");

    var regex159 = /^(a){0,2}/;
    var input0 = "bcd";
    var results = ["", undefined];
    shouldBe("regex159.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex159.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex159.exec(input2);", "results");

    var regex160 = /^(a){0,3}/;
    var input0 = "bcd";
    var results = ["", undefined];
    shouldBe("regex160.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex160.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex160.exec(input2);", "results");
    var input3 = "aaa";
    var results = ["aaa", "a"];
    shouldBe("regex160.exec(input3);", "results");

    var regex161 = /^(a){0,}/;
    var input0 = "bcd";
    var results = ["", undefined];
    shouldBe("regex161.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex161.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex161.exec(input2);", "results");
    var input3 = "aaa";
    var results = ["aaa", "a"];
    shouldBe("regex161.exec(input3);", "results");
    var input4 = "aaaaaaaa";
    var results = ["aaaaaaaa", "a"];
    shouldBe("regex161.exec(input4);", "results");

    var regex162 = /^(a){1,1}/;
    var input0 = "bcd";
    var results = null;
    shouldBe("regex162.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex162.exec(input1);", "results");
    var input2 = "aab";
    var results = ["a", "a"];
    shouldBe("regex162.exec(input2);", "results");

    var regex163 = /^(a){1,2}/;
    var input0 = "bcd";
    var results = null;
    shouldBe("regex163.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex163.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex163.exec(input2);", "results");

    var regex164 = /^(a){1,3}/;
    var input0 = "bcd";
    var results = null;
    shouldBe("regex164.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex164.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex164.exec(input2);", "results");
    var input3 = "aaa";
    var results = ["aaa", "a"];
    shouldBe("regex164.exec(input3);", "results");

    var regex165 = /^(a){1,}/;
    var input0 = "bcd";
    var results = null;
    shouldBe("regex165.exec(input0);", "results");
    var input1 = "abc";
    var results = ["a", "a"];
    shouldBe("regex165.exec(input1);", "results");
    var input2 = "aab";
    var results = ["aa", "a"];
    shouldBe("regex165.exec(input2);", "results");
    var input3 = "aaa";
    var results = ["aaa", "a"];
    shouldBe("regex165.exec(input3);", "results");
    var input4 = "aaaaaaaa";
    var results = ["aaaaaaaa", "a"];
    shouldBe("regex165.exec(input4);", "results");

    var regex166 = /.*\.gif/;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["bib.gif"];
    shouldBe("regex166.exec(input0);", "results");

    var regex167 = /.{0,}\.gif/;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["bib.gif"];
    shouldBe("regex167.exec(input0);", "results");

    var regex168 = /.*\.gif/m;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["bib.gif"];
    shouldBe("regex168.exec(input0);", "results");

    var regex169 = /.*\.gif/;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["bib.gif"];
    shouldBe("regex169.exec(input0);", "results");

    var regex170 = /.*\.gif/m;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["bib.gif"];
    shouldBe("regex170.exec(input0);", "results");

    var regex171 = /.*$/;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["no"];
    shouldBe("regex171.exec(input0);", "results");

    var regex172 = /.*$/m;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["borfle"];
    shouldBe("regex172.exec(input0);", "results");

    var regex173 = /[\w\W]*?$/;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["borfle\nbib.gif\x0ano"];
    shouldBe("regex173.exec(input0);", "results");

    var regex174 = /[\w\W]*?$/m;
    var input0 = "borfle\nbib.gif\nno";
    var results = ["borfle"];
    shouldBe("regex174.exec(input0);", "results");

    var regex175 = /.*$/;
    var input0 = "borfle\nbib.gif\nno\n";
    var results = [""];
    shouldBe("regex175.exec(input0);", "results");

    var regex176 = /.*$/m;
    var input0 = "borfle\nbib.gif\nno\n";
    var results = ["borfle"];
    shouldBe("regex176.exec(input0);", "results");

    var regex177 = /.*$/;
    var input0 = "borfle\nbib.gif\nno\n";
    var results = [""];
    shouldBe("regex177.exec(input0);", "results");

    var regex178 = /.*$/m;
    var input0 = "borfle\nbib.gif\nno\n";
    var results = ["borfle"];
    shouldBe("regex178.exec(input0);", "results");

    var regex179 = /(.*X|^B)/;
    var input0 = "abcde\n1234Xyz";
    var results = ["1234X", "1234X"];
    shouldBe("regex179.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B", "B"];
    shouldBe("regex179.exec(input1);", "results");
    // Failers
    var input2 = "abcde\nBar";
    var results = null;
    shouldBe("regex179.exec(input2);", "results");

    var regex180 = /(.*X|^B)/m;
    var input0 = "abcde\n1234Xyz";
    var results = ["1234X", "1234X"];
    shouldBe("regex180.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B", "B"];
    shouldBe("regex180.exec(input1);", "results");
    var input2 = "abcde\nBar";
    var results = ["B", "B"];
    shouldBe("regex180.exec(input2);", "results");

    var regex181 = /([\w\W]*X|^B)/;
    var input0 = "abcde\n1234Xyz";
    var results = ["abcde\x0a1234X", "abcde\x0a1234X"];
    shouldBe("regex181.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B", "B"];
    shouldBe("regex181.exec(input1);", "results");
    // Failers
    var input2 = "abcde\nBar";
    var results = null;
    shouldBe("regex181.exec(input2);", "results");

    var regex182 = /([\w\W]*X|^B)/m;
    var input0 = "abcde\n1234Xyz";
    var results = ["abcde\x0a1234X", "abcde\x0a1234X"];
    shouldBe("regex182.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B", "B"];
    shouldBe("regex182.exec(input1);", "results");
    var input2 = "abcde\nBar";
    var results = ["B", "B"];
    shouldBe("regex182.exec(input2);", "results");

    var regex183 = /([\w\W]*X|^B)/;
    var input0 = "abcde\n1234Xyz";
    var results = ["abcde\x0a1234X", "abcde\x0a1234X"];
    shouldBe("regex183.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B", "B"];
    shouldBe("regex183.exec(input1);", "results");
    // Failers
    var input2 = "abcde\nBar";
    var results = null;
    shouldBe("regex183.exec(input2);", "results");

    var regex184 = /(?:.*X|^B)/;
    var input0 = "abcde\n1234Xyz";
    var results = ["1234X"];
    shouldBe("regex184.exec(input0);", "results");
    var input1 = "BarFoo";
    var results = ["B"];
    shouldBe("regex184.exec(input1);", "results");
    // Failers
    var input2 = "abcde\nBar";
    var results = null;
    shouldBe("regex184.exec(input2);", "results");

    var regex185 = /^.*B/;
    // Failers
    var input0 = "abc\nB";
    var results = null;
    shouldBe("regex185.exec(input0);", "results");

    var regex186 = /^[\w\W]*B/;
    var input0 = "abc\nB";
    var results = ["abc\x0aB"];
    shouldBe("regex186.exec(input0);", "results");

    var regex187 = /.*B/;
    var input0 = "abc\nB";
    var results = ["B"];
    shouldBe("regex187.exec(input0);", "results");

    var regex188 = /^.*B/;
    // Failers
    var input0 = "abc\nB";
    var results = null;
    shouldBe("regex188.exec(input0);", "results");

    var regex189a = /^B/;
    // Failers
    var input0 = "abc\nB";
    var results = null;
    shouldBe("regex189a.exec(input0);", "results");

    var regex189b = /^B/m;
    var input0 = "abc\nB";
    var results = ["B"];
    shouldBe("regex189b.exec(input0);", "results");

    var regex190a = /B$/;
    // Failers
    var input0 = "B\n";
    var results = null;
    shouldBe("regex190a.exec(input0);", "results");

    var regex190b = /B$/m;
    var input0 = "B\n";
    var results = ["B"];
    shouldBe("regex190b.exec(input0);", "results");

    var regex191 = /^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]/;
    var input0 = "123456654321";
    var results = ["123456654321"];
    shouldBe("regex191.exec(input0);", "results");

    var regex192 = /^\d\d\d\d\d\d\d\d\d\d\d\d/;
    var input0 = "123456654321";
    var results = ["123456654321"];
    shouldBe("regex192.exec(input0);", "results");

    var regex193 = /^[\d][\d][\d][\d][\d][\d][\d][\d][\d][\d][\d][\d]/;
    var input0 = "123456654321";
    var results = ["123456654321"];
    shouldBe("regex193.exec(input0);", "results");

    var regex194 = /^[abc]{12}/;
    var input0 = "abcabcabcabc";
    var results = ["abcabcabcabc"];
    shouldBe("regex194.exec(input0);", "results");

    var regex195 = /^[a-c]{12}/;
    var input0 = "abcabcabcabc";
    var results = ["abcabcabcabc"];
    shouldBe("regex195.exec(input0);", "results");

    var regex196 = /^(a|b|c){12}/;
    var input0 = "abcabcabcabc";
    var results = ["abcabcabcabc", "c"];
    shouldBe("regex196.exec(input0);", "results");

    var regex197 = /^[abcdefghijklmnopqrstuvwxy0123456789]/;
    var input0 = "n";
    var results = ["n"];
    shouldBe("regex197.exec(input0);", "results");
    // Failers
    var input1 = "z";
    var results = null;
    shouldBe("regex197.exec(input1);", "results");

    var regex198 = /abcde{0,0}/;
    var input0 = "abcd";
    var results = ["abcd"];
    shouldBe("regex198.exec(input0);", "results");
    // Failers
    var input1 = "abce";
    var results = null;
    shouldBe("regex198.exec(input1);", "results");

    var regex199 = /ab[cd]{0,0}e/;
    var input0 = "abe";
    var results = ["abe"];
    shouldBe("regex199.exec(input0);", "results");
    // Failers
    var input1 = "abcde";
    var results = null;
    shouldBe("regex199.exec(input1);", "results");

    var regex200 = /ab(c){0,0}d/;
    var input0 = "abd";
    var results = ["abd", undefined];
    shouldBe("regex200.exec(input0);", "results");
    // Failers
    var input1 = "abcd";
    var results = null;
    shouldBe("regex200.exec(input1);", "results");

    var regex201 = /a(b*)/;
    var input0 = "a";
    var results = ["a", ""];
    shouldBe("regex201.exec(input0);", "results");
    var input1 = "ab";
    var results = ["ab", "b"];
    shouldBe("regex201.exec(input1);", "results");
    var input2 = "abbbb";
    var results = ["abbbb", "bbbb"];
    shouldBe("regex201.exec(input2);", "results");
    // Failers
    var input3 = "bbbbb";
    var results = null;
    shouldBe("regex201.exec(input3);", "results");

    var regex202 = /ab\d{0}e/;
    var input0 = "abe";
    var results = ["abe"];
    shouldBe("regex202.exec(input0);", "results");
    // Failers
    var input1 = "ab1e";
    var results = null;
    shouldBe("regex202.exec(input1);", "results");

    var regex203 = /"([^\\"]+|\\.)*"/;
    var input0 = 'the "quick" brown fox';
    var results = ['"quick"', "quick"];
    shouldBe("regex203.exec(input0);", "results");
    var input1 = '"the \\"quick\\" brown fox"';
    var results = ['"the \\"quick\\" brown fox"', " brown fox"];
    shouldBe("regex203.exec(input1);", "results");

    var regex204 =
        /<tr([\w\W\s\d][^<>]{0,})><TD([\w\W\s\d][^<>]{0,})>([\d]{0,}\.)(.*)((<BR>([\w\W\s\d][^<>]{0,})|[\s]{0,}))<\/a><\/TD><TD([\w\W\s\d][^<>]{0,})>([\w\W\s\d][^<>]{0,})<\/TD><TD([\w\W\s\d][^<>]{0,})>([\w\W\s\d][^<>]{0,})<\/TD><\/TR>/i;
    var input0 =
        "<TR BGCOLOR='#DBE9E9'><TD align=left valign=top>43.<a href='joblist.cfm?JobID=94 6735&Keyword='>Word Processor<BR>(N-1286)</a></TD><TD align=left valign=top>Lega lstaff.com</TD><TD align=left valign=top>CA - Statewide</TD></TR>";
    var results = [
        "<TR BGCOLOR=\'#DBE9E9\'><TD align=left valign=top>43.<a href=\'joblist.cfm?JobID=94 6735&Keyword=\'>Word Processor<BR>(N-1286)</a></TD><TD align=left valign=top>Lega lstaff.com</TD><TD align=left valign=top>CA - Statewide</TD></TR>",
        " BGCOLOR=\'#DBE9E9\'",
        " align=left valign=top",
        "43.",
        "<a href=\'joblist.cfm?JobID=94 6735&Keyword=\'>Word Processor<BR>(N-1286)",
        "",
        "",
        undefined,
        " align=left valign=top",
        "Lega lstaff.com",
        " align=left valign=top",
        "CA - Statewide",
    ];
    shouldBe("regex204.exec(input0);", "results");

    var regex205 = /a[^a]b/;
    var input0 = "acb";
    var results = ["acb"];
    shouldBe("regex205.exec(input0);", "results");
    var input1 = "a\nb";
    var results = ["a\x0ab"];
    shouldBe("regex205.exec(input1);", "results");

    var regex206 = /a.b/;
    var input0 = "acb";
    var results = ["acb"];
    shouldBe("regex206.exec(input0);", "results");
    // Failers
    var input1 = "a\nb";
    var results = null;
    shouldBe("regex206.exec(input1);", "results");

    var regex207 = /a[^a]b/;
    var input0 = "acb";
    var results = ["acb"];
    shouldBe("regex207.exec(input0);", "results");
    var input1 = "a\nb";
    var results = ["a\x0ab"];
    shouldBe("regex207.exec(input1);", "results");

    var regex208 = /a[\w\W]b/;
    var input0 = "acb";
    var results = ["acb"];
    shouldBe("regex208.exec(input0);", "results");
    var input1 = "a\nb";
    var results = ["a\x0ab"];
    shouldBe("regex208.exec(input1);", "results");

    var regex209 = /^(b+?|a){1,2}?c/;
    var input0 = "bac";
    var results = ["bac", "a"];
    shouldBe("regex209.exec(input0);", "results");
    var input1 = "bbac";
    var results = ["bbac", "a"];
    shouldBe("regex209.exec(input1);", "results");
    var input2 = "bbbac";
    var results = ["bbbac", "a"];
    shouldBe("regex209.exec(input2);", "results");
    var input3 = "bbbbac";
    var results = ["bbbbac", "a"];
    shouldBe("regex209.exec(input3);", "results");
    var input4 = "bbbbbac";
    var results = ["bbbbbac", "a"];
    shouldBe("regex209.exec(input4);", "results");

    var regex210 = /^(b+|a){1,2}?c/;
    var input0 = "bac";
    var results = ["bac", "a"];
    shouldBe("regex210.exec(input0);", "results");
    var input1 = "bbac";
    var results = ["bbac", "a"];
    shouldBe("regex210.exec(input1);", "results");
    var input2 = "bbbac";
    var results = ["bbbac", "a"];
    shouldBe("regex210.exec(input2);", "results");
    var input3 = "bbbbac";
    var results = ["bbbbac", "a"];
    shouldBe("regex210.exec(input3);", "results");
    var input4 = "bbbbbac";
    var results = ["bbbbbac", "a"];
    shouldBe("regex210.exec(input4);", "results");

    var regex211 = /(?!^)x/m;
    var input0 = "a\bx\n";
    var results = ["x"];
    shouldBe("regex211.exec(input0);", "results");
    // Failers
    var input1 = "x\nb\n";
    var results = null;
    shouldBe("regex211.exec(input1);", "results");

    var regex212 = /\0{ab}/;
    var input0 = "\0{ab}";
    var results = ["\x00{ab}"];
    shouldBe("regex212.exec(input0);", "results");

    var regex213 = /(A|B)*?CD/;
    var input0 = "CD";
    var results = ["CD", undefined];
    shouldBe("regex213.exec(input0);", "results");

    var regex214 = /(A|B)*CD/;
    var input0 = "CD";
    var results = ["CD", undefined];
    shouldBe("regex214.exec(input0);", "results");

    var regex215 = /^((AB)+?)\2$/;
    var input0 = "ABABAB";
    var results = ["ABABAB", "ABAB", "AB"];
    shouldBe("regex215.exec(input0);", "results");

    var regex216 = /(AB)*\1/;
    var input0 = "ABABAB";
    var results = ["ABABAB", "AB"];
    shouldBe("regex216.exec(input0);", "results");

    var regex220 = /^abc$/m;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex220.exec(input0);", "results");
    var input1 = "abc\n";
    var results = ["abc"];
    shouldBe("regex220.exec(input1);", "results");
    var input2 = "qqq\nabc";
    var results = ["abc"];
    shouldBe("regex220.exec(input2);", "results");
    var input3 = "abc\nzzz";
    var results = ["abc"];
    shouldBe("regex220.exec(input3);", "results");
    var input4 = "qqq\nabc\nzzz";
    var results = ["abc"];
    shouldBe("regex220.exec(input4);", "results");

    var regex225 = /(\d+)(\w)/;
    var input0 = "12345a";
    var results = ["12345a", "12345", "a"];
    shouldBe("regex225.exec(input0);", "results");
    var input1 = "12345+";
    var results = ["12345", "1234", "5"];
    shouldBe("regex225.exec(input1);", "results");

    var regex234 = /ab/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex234.exec(input0);", "results");
    // Failers
    var input1 = "Ab";
    var results = null;
    shouldBe("regex234.exec(input1);", "results");
    var input2 = "aB";
    var results = null;
    shouldBe("regex234.exec(input2);", "results");
    var input3 = "AB";
    var results = null;
    shouldBe("regex234.exec(input3);", "results");

    var regex235 = /(a bc)d e/;
    var input0 = "a bcd e";
    var results = ["a bcd e", "a bc"];
    shouldBe("regex235.exec(input0);", "results");
    // Failers
    var input1 = "a b cd e";
    var results = null;
    shouldBe("regex235.exec(input1);", "results");
    var input2 = "abcd e";
    var results = null;
    shouldBe("regex235.exec(input2);", "results");
    var input3 = "a bcde";
    var results = null;
    shouldBe("regex235.exec(input3);", "results");

    var regex236 = /(a bcde f)/;
    var input0 = "a bcde f";
    var results = ["a bcde f", "a bcde f"];
    shouldBe("regex236.exec(input0);", "results");
    // Failers
    var input1 = "abcdef";
    var results = null;
    shouldBe("regex236.exec(input1);", "results");

    var regex237 = /(a[bB])c/;
    var input0 = "abc";
    var results = ["abc", "ab"];
    shouldBe("regex237.exec(input0);", "results");
    var input1 = "aBc";
    var results = ["aBc", "aB"];
    shouldBe("regex237.exec(input1);", "results");
    // Failers
    var input2 = "abC";
    var results = null;
    shouldBe("regex237.exec(input2);", "results");
    var input3 = "aBC";
    var results = null;
    shouldBe("regex237.exec(input3);", "results");
    var input4 = "Abc";
    var results = null;
    shouldBe("regex237.exec(input4);", "results");
    var input5 = "ABc";
    var results = null;
    shouldBe("regex237.exec(input5);", "results");
    var input6 = "ABC";
    var results = null;
    shouldBe("regex237.exec(input6);", "results");
    var input7 = "AbC";
    var results = null;
    shouldBe("regex237.exec(input7);", "results");

    var regex238 = /a[bB]c/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex238.exec(input0);", "results");
    var input1 = "aBc";
    var results = ["aBc"];
    shouldBe("regex238.exec(input1);", "results");
    // Failers
    var input2 = "ABC";
    var results = null;
    shouldBe("regex238.exec(input2);", "results");
    var input3 = "abC";
    var results = null;
    shouldBe("regex238.exec(input3);", "results");
    var input4 = "aBC";
    var results = null;
    shouldBe("regex238.exec(input4);", "results");

    var regex239 = /a[bB]*c/;
    var input0 = "aBc";
    var results = ["aBc"];
    shouldBe("regex239.exec(input0);", "results");
    var input1 = "aBBc";
    var results = ["aBBc"];
    shouldBe("regex239.exec(input1);", "results");
    // Failers
    var input2 = "aBC";
    var results = null;
    shouldBe("regex239.exec(input2);", "results");
    var input3 = "aBBC";
    var results = null;
    shouldBe("regex239.exec(input3);", "results");

    var regex240 = /a(?=b[cC])\w\wd/;
    var input0 = "abcd";
    var results = ["abcd"];
    shouldBe("regex240.exec(input0);", "results");
    var input1 = "abCd";
    var results = ["abCd"];
    shouldBe("regex240.exec(input1);", "results");
    // Failers
    var input2 = "aBCd";
    var results = null;
    shouldBe("regex240.exec(input2);", "results");
    var input3 = "abcD";
    var results = null;
    shouldBe("regex240.exec(input3);", "results");

    var regex241 = /(?:more[\w\W]*than).*million/i;
    var input0 = "more than million";
    var results = ["more than million"];
    shouldBe("regex241.exec(input0);", "results");
    var input1 = "more than MILLION";
    var results = ["more than MILLION"];
    shouldBe("regex241.exec(input1);", "results");
    var input2 = "more \n than Million";
    var results = ["more \x0a than Million"];
    shouldBe("regex241.exec(input2);", "results");
    var input3 = "MORE THAN MILLION";
    var results = ["MORE THAN MILLION"];
    // Failers
    shouldBe("regex241.exec(input3);", "results");
    var input4 = "more \n than \n million";
    var results = null;
    shouldBe("regex241.exec(input4);", "results");

    var regex242 = /(?:more[\w\W]*than).*million/i;
    var input0 = "more than million";
    var results = ["more than million"];
    shouldBe("regex242.exec(input0);", "results");
    var input1 = "more than MILLION";
    var results = ["more than MILLION"];
    shouldBe("regex242.exec(input1);", "results");
    var input2 = "more \n than Million";
    var results = ["more \x0a than Million"];
    shouldBe("regex242.exec(input2);", "results");
    var input3 = "MORE THAN MILLION";
    var results = ["MORE THAN MILLION"];
    // Failers
    shouldBe("regex242.exec(input3);", "results");
    var input4 = "more \n than \n million";
    var results = null;
    shouldBe("regex242.exec(input4);", "results");

    var regex243 = /(?:ab+)+c/i;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex243.exec(input0);", "results");
    var input1 = "aBbc";
    var results = ["aBbc"];
    shouldBe("regex243.exec(input1);", "results");
    var input2 = "aBBc";
    var results = ["aBBc"];
    shouldBe("regex243.exec(input2);", "results");
    var input3 = "Abc";
    var results = ["Abc"];
    shouldBe("regex243.exec(input3);", "results");
    var input4 = "abbC";
    var results = ["abbC"];
    shouldBe("regex243.exec(input4);", "results");
    // Failers
    var input5 = "abAb";
    var results = null;
    shouldBe("regex243.exec(input5);", "results");

    var regex244 = /(?=a[bB])\w\wc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex244.exec(input0);", "results");
    var input1 = "aBc";
    var results = ["aBc"];
    shouldBe("regex244.exec(input1);", "results");
    // Failers
    var input2 = "Ab";
    var results = null;
    shouldBe("regex244.exec(input2);", "results");
    var input3 = "abC";
    var results = null;
    shouldBe("regex244.exec(input3);", "results");
    var input4 = "aBC";
    var results = null;
    shouldBe("regex244.exec(input4);", "results");

    var regex246 = /(?:(a)|b)(?:A|B)/;
    var input0 = "aA";
    var results = ["aA", "a"];
    shouldBe("regex246.exec(input0);", "results");
    var input1 = "bB";
    var results = ["bB", undefined];
    shouldBe("regex246.exec(input1);", "results");
    var input2 = "aB";
    var results = ["aB", "a"];
    shouldBe("regex246.exec(input2);", "results");
    var input3 = "bA";
    var results = ["bA", undefined];
    shouldBe("regex246.exec(input3);", "results");

    var regex247 = /^(a)?(?:a|b)+$/;
    var input0 = "aa";
    var results = ["aa", "a"];
    shouldBe("regex247.exec(input0);", "results");
    var input1 = "b";
    var results = ["b", undefined];
    shouldBe("regex247.exec(input1);", "results");
    var input2 = "bb";
    var results = ["bb", undefined];
    shouldBe("regex247.exec(input2);", "results");
    var input3 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex247.exec(input3);", "results");

    var regex248 = /^(?:(?=abc)\w{3}:|\d\d)$/;
    var input0 = "abc:";
    var results = ["abc:"];
    shouldBe("regex248.exec(input0);", "results");
    var input1 = "12";
    var results = ["12"];
    shouldBe("regex248.exec(input1);", "results");
    // Failers
    var input2 = "123";
    var results = null;
    shouldBe("regex248.exec(input2);", "results");
    var input3 = "xyz";
    var results = null;
    shouldBe("regex248.exec(input3);", "results");

    var regex249 = /^(?:(?!abc)\d\d|\w{3}:)$/;
    var input0 = "abc:";
    var results = ["abc:"];
    shouldBe("regex249.exec(input0);", "results");
    var input1 = "12";
    var results = ["12"];
    shouldBe("regex249.exec(input1);", "results");
    // Failers
    var input2 = "123";
    var results = null;
    shouldBe("regex249.exec(input2);", "results");
    var input3 = "xyz";
    var results = null;
    shouldBe("regex249.exec(input3);", "results");

    var regex252 = /(?=\()(\()[^()]+\)|[^()]+/;
    var input0 = "abcd";
    var results = ["abcd", undefined];
    shouldBe("regex252.exec(input0);", "results");
    var input1 = "(abcd)";
    var results = ["(abcd)", "("];
    shouldBe("regex252.exec(input1);", "results");
    var input2 = "the quick (abcd) fox";
    var results = ["the quick ", undefined];
    shouldBe("regex252.exec(input2);", "results");
    var input3 = "(abcd";
    var results = ["abcd", undefined];
    shouldBe("regex252.exec(input3);", "results");

    var regex253 = /^(?:a|(1)(2))+$/;
    var input0 = "12";
    var results = ["12", "1", "2"];
    shouldBe("regex253.exec(input0);", "results");
    var input1 = "12a";
    var results = ["12a", undefined, undefined];
    shouldBe("regex253.exec(input1);", "results");
    var input2 = "12aa";
    var results = ["12aa", undefined, undefined];
    shouldBe("regex253.exec(input2);", "results");
    // Failers
    var input3 = "1234";
    var results = null;
    shouldBe("regex253.exec(input3);", "results");

    var regex254 = /(blah)\s+\1/i;
    var input0 = "blah blah";
    var results = ["blah blah", "blah"];
    shouldBe("regex254.exec(input0);", "results");
    var input1 = "BLAH BLAH";
    var results = ["BLAH BLAH", "BLAH"];
    shouldBe("regex254.exec(input1);", "results");
    var input2 = "Blah Blah";
    var results = ["Blah Blah", "Blah"];
    shouldBe("regex254.exec(input2);", "results");
    var input3 = "blaH blaH";
    var results = ["blaH blaH", "blaH"];
    shouldBe("regex254.exec(input3);", "results");
    var input4 = "blah BLAH";
    var results = ["blah BLAH", "blah"];
    shouldBe("regex254.exec(input4);", "results");
    var input5 = "Blah blah";
    var results = ["Blah blah", "Blah"];
    shouldBe("regex254.exec(input5);", "results");
    var input6 = "blaH blah";
    var results = ["blaH blah", "blaH"];
    shouldBe("regex254.exec(input6);", "results");

    var regex255 = /(blah)\s+(?:\1)/i;
    var input0 = "blah blah";
    var results = ["blah blah", "blah"];
    shouldBe("regex255.exec(input0);", "results");
    var input1 = "BLAH BLAH";
    var results = ["BLAH BLAH", "BLAH"];
    shouldBe("regex255.exec(input1);", "results");
    var input2 = "Blah Blah";
    var results = ["Blah Blah", "Blah"];
    shouldBe("regex255.exec(input2);", "results");
    var input3 = "blaH blaH";
    var results = ["blaH blaH", "blaH"];
    shouldBe("regex255.exec(input3);", "results");
    var input4 = "blah BLAH";
    var results = ["blah BLAH", "blah"];
    shouldBe("regex255.exec(input4);", "results");
    var input5 = "Blah blah";
    var results = ["Blah blah", "Blah"];
    shouldBe("regex255.exec(input5);", "results");
    var input6 = "blaH blah";
    var results = ["blaH blah", "blaH"];
    shouldBe("regex255.exec(input6);", "results");

    var regex257 = /(abc|)+/;
    var input0 = "abc";
    var results = ["abc", "abc"];
    shouldBe("regex257.exec(input0);", "results");
    var input1 = "abcabc";
    var results = ["abcabc", "abc"];
    shouldBe("regex257.exec(input1);", "results");
    var input2 = "abcabcabc";
    var results = ["abcabcabc", "abc"];
    shouldBe("regex257.exec(input2);", "results");
    var input3 = "xyz";
    var results = ["", ""];
    shouldBe("regex257.exec(input3);", "results");

    var regex258 = /([a]*)*/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex258.exec(input0);", "results");
    var input1 = "aaaaa";
    var results = ["aaaaa", "aaaaa"];
    shouldBe("regex258.exec(input1);", "results");

    var regex259 = /([ab]*)*/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex259.exec(input0);", "results");
    var input1 = "b";
    var results = ["b", "b"];
    shouldBe("regex259.exec(input1);", "results");
    var input2 = "ababab";
    var results = ["ababab", "ababab"];
    shouldBe("regex259.exec(input2);", "results");
    var input3 = "aaaabcde";
    var results = ["aaaab", "aaaab"];
    shouldBe("regex259.exec(input3);", "results");
    var input4 = "bbbb";
    var results = ["bbbb", "bbbb"];
    shouldBe("regex259.exec(input4);", "results");

    var regex260 = /([^a]*)*/;
    var input0 = "b";
    var results = ["b", "b"];
    shouldBe("regex260.exec(input0);", "results");
    var input1 = "bbbb";
    var results = ["bbbb", "bbbb"];
    shouldBe("regex260.exec(input1);", "results");
    var input2 = "aaa";
    var results = ["", undefined];
    shouldBe("regex260.exec(input2);", "results");

    var regex261 = /([^ab]*)*/;
    var input0 = "cccc";
    var results = ["cccc", "cccc"];
    shouldBe("regex261.exec(input0);", "results");
    var input1 = "abab";
    var results = ["", undefined];
    shouldBe("regex261.exec(input1);", "results");

    var regex262 = /([a]*?)*/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex262.exec(input0);", "results");
    var input1 = "aaaa";
    var results = ["aaaa", "a"];
    shouldBe("regex262.exec(input1);", "results");

    var regex263 = /([ab]*?)*/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex263.exec(input0);", "results");
    var input1 = "b";
    var results = ["b", "b"];
    shouldBe("regex263.exec(input1);", "results");
    var input2 = "abab";
    var results = ["abab", "b"];
    shouldBe("regex263.exec(input2);", "results");
    var input3 = "baba";
    var results = ["baba", "a"];
    shouldBe("regex263.exec(input3);", "results");

    var regex264 = /([^a]*?)*/;
    var input0 = "b";
    var results = ["b", "b"];
    shouldBe("regex264.exec(input0);", "results");
    var input1 = "bbbb";
    var results = ["bbbb", "b"];
    shouldBe("regex264.exec(input1);", "results");
    var input2 = "aaa";
    var results = ["", undefined];
    shouldBe("regex264.exec(input2);", "results");

    var regex265 = /([^ab]*?)*/;
    var input0 = "c";
    var results = ["c", "c"];
    shouldBe("regex265.exec(input0);", "results");
    var input1 = "cccc";
    var results = ["cccc", "c"];
    shouldBe("regex265.exec(input1);", "results");
    var input2 = "baba";
    var results = ["", undefined];
    shouldBe("regex265.exec(input2);", "results");

    var regex269 = /(?=[^a-z]+[a-z])\d{2}-[a-z]{3}-\d{2}|(?![^a-z]+[a-z])\d{2}-\d{2}-\d{2}/;
    var input0 = "12-sep-98";
    var results = ["12-sep-98"];
    shouldBe("regex269.exec(input0);", "results");
    var input1 = "12-09-98";
    var results = ["12-09-98"];
    shouldBe("regex269.exec(input1);", "results");
    var input2 = "sep-12-98";
    var results = null;
    shouldBe("regex269.exec(input2);", "results");

    var regex271 = /(?:saturday|sunday)/i;
    var input0 = "saturday";
    var results = ["saturday"];
    shouldBe("regex271.exec(input0);", "results");
    var input1 = "sunday";
    var results = ["sunday"];
    shouldBe("regex271.exec(input1);", "results");
    var input2 = "Saturday";
    var results = ["Saturday"];
    shouldBe("regex271.exec(input2);", "results");
    var input3 = "Sunday";
    var results = ["Sunday"];
    shouldBe("regex271.exec(input3);", "results");
    var input4 = "SATURDAY";
    var results = ["SATURDAY"];
    shouldBe("regex271.exec(input4);", "results");
    var input5 = "SUNDAY";
    var results = ["SUNDAY"];
    shouldBe("regex271.exec(input5);", "results");
    var input6 = "SunDay";
    var results = ["SunDay"];
    shouldBe("regex271.exec(input6);", "results");

    var regex272 = /([aA][bB][cC]|[bB][bB])x/;
    var input0 = "abcx";
    var results = ["abcx", "abc"];
    shouldBe("regex272.exec(input0);", "results");
    var input1 = "aBCx";
    var results = ["aBCx", "aBC"];
    shouldBe("regex272.exec(input1);", "results");
    var input2 = "bbx";
    var results = ["bbx", "bb"];
    shouldBe("regex272.exec(input2);", "results");
    var input3 = "BBx";
    var results = ["BBx", "BB"];
    shouldBe("regex272.exec(input3);", "results");
    // Failers
    var input4 = "abcX";
    var results = null;
    shouldBe("regex272.exec(input4);", "results");
    var input5 = "aBCX";
    var results = null;
    shouldBe("regex272.exec(input5);", "results");
    var input6 = "bbX";
    var results = null;
    shouldBe("regex272.exec(input6);", "results");
    var input7 = "BBX";
    var results = null;
    shouldBe("regex272.exec(input7);", "results");

    var regex273 = /^([ab][cd]|[ef])/i;
    var input0 = "ac";
    var results = ["ac", "ac"];
    shouldBe("regex273.exec(input0);", "results");
    var input1 = "aC";
    var results = ["aC", "aC"];
    shouldBe("regex273.exec(input1);", "results");
    var input2 = "bD";
    var results = ["bD", "bD"];
    shouldBe("regex273.exec(input2);", "results");
    var input3 = "elephant";
    var results = ["e", "e"];
    shouldBe("regex273.exec(input3);", "results");
    var input4 = "Europe";
    var results = ["E", "E"];
    shouldBe("regex273.exec(input4);", "results");
    var input5 = "frog";
    var results = ["f", "f"];
    shouldBe("regex273.exec(input5);", "results");
    var input6 = "France";
    var results = ["F", "F"];
    shouldBe("regex273.exec(input6);", "results");
    // Failers
    var input7 = "Africa";
    var results = null;
    shouldBe("regex273.exec(input7);", "results");

    var regex274 = /^(ab|a[b-cB-C]d|x[yY]|[zZ])/;
    var input0 = "ab";
    var results = ["ab", "ab"];
    shouldBe("regex274.exec(input0);", "results");
    var input1 = "aBd";
    var results = ["aBd", "aBd"];
    shouldBe("regex274.exec(input1);", "results");
    var input2 = "xy";
    var results = ["xy", "xy"];
    shouldBe("regex274.exec(input2);", "results");
    var input3 = "xY";
    var results = ["xY", "xY"];
    shouldBe("regex274.exec(input3);", "results");
    var input4 = "zebra";
    var results = ["z", "z"];
    shouldBe("regex274.exec(input4);", "results");
    var input5 = "Zambesi";
    var results = ["Z", "Z"];
    shouldBe("regex274.exec(input5);", "results");
    // Failers
    var input6 = "aCD";
    var results = null;
    shouldBe("regex274.exec(input6);", "results");
    var input7 = "XY";
    var results = null;
    shouldBe("regex274.exec(input7);", "results");

    var regex277 = /^(a\1?){4}$/;
    var input0 = "a";
    var results = null;
    shouldBe("regex277.exec(input0);", "results");
    var input1 = "aa";
    var results = null;
    shouldBe("regex277.exec(input1);", "results");
    var input2 = "aaa";
    var results = null;
    shouldBe("regex277.exec(input2);", "results");
    var input3 = "aaaa";
    var results = ["aaaa", "a"];
    shouldBe("regex277.exec(input3);", "results");
    var input4 = "aaaaa";
    var results = null;
    shouldBe("regex277.exec(input4);", "results");
    var input5 = "aaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input5);", "results");
    var input6 = "aaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input6);", "results");
    var input7 = "aaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input7);", "results");
    var input8 = "aaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input8);", "results");
    var input9 = "aaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input9);", "results");
    var input10 = "aaaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input10);", "results");
    var input11 = "aaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input11);", "results");
    var input12 = "aaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input12);", "results");
    var input13 = "aaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input13);", "results");
    var input14 = "aaaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex277.exec(input14);", "results");

    var regex278 = /^(a\1?)(a\1?)(a\2?)(a\3?)$/;
    var input0 = "a";
    var results = null;
    shouldBe("regex278.exec(input0);", "results");
    var input1 = "aa";
    var results = null;
    shouldBe("regex278.exec(input1);", "results");
    var input2 = "aaa";
    var results = null;
    shouldBe("regex278.exec(input2);", "results");
    var input3 = "aaaa";
    var results = ["aaaa", "a", "a", "a", "a"];
    shouldBe("regex278.exec(input3);", "results");
    var input4 = "aaaaa";
    var results = ["aaaaa", "a", "aa", "a", "a"];
    shouldBe("regex278.exec(input4);", "results");
    var input5 = "aaaaaa";
    var results = ["aaaaaa", "a", "aa", "a", "aa"];
    shouldBe("regex278.exec(input5);", "results");
    var input6 = "aaaaaaa";
    var results = ["aaaaaaa", "a", "aa", "aaa", "a"];
    shouldBe("regex278.exec(input6);", "results");
    var input7 = "aaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input7);", "results");
    var input8 = "aaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input8);", "results");
    var input9 = "aaaaaaaaaa";
    var results = ["aaaaaaaaaa", "a", "aa", "aaa", "aaaa"];
    shouldBe("regex278.exec(input9);", "results");
    var input10 = "aaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input10);", "results");
    var input11 = "aaaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input11);", "results");
    var input12 = "aaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input12);", "results");
    var input13 = "aaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input13);", "results");
    var input14 = "aaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input14);", "results");
    var input15 = "aaaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex278.exec(input15);", "results");

    var regex279 = /abc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex279.exec(input0);", "results");
    var input1 = "xabcy";
    var results = ["abc"];
    shouldBe("regex279.exec(input1);", "results");
    var input2 = "ababc";
    var results = ["abc"];
    shouldBe("regex279.exec(input2);", "results");
    // Failers
    var input3 = "xbc";
    var results = null;
    shouldBe("regex279.exec(input3);", "results");
    var input4 = "axc";
    var results = null;
    shouldBe("regex279.exec(input4);", "results");
    var input5 = "abx";
    var results = null;
    shouldBe("regex279.exec(input5);", "results");

    var regex280 = /ab*c/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex280.exec(input0);", "results");

    var regex281 = /ab*bc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex281.exec(input0);", "results");
    var input1 = "abbc";
    var results = ["abbc"];
    shouldBe("regex281.exec(input1);", "results");
    var input2 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex281.exec(input2);", "results");

    var regex282 = /.{1}/;
    var input0 = "abbbbc";
    var results = ["a"];
    shouldBe("regex282.exec(input0);", "results");

    var regex283 = /.{3,4}/;
    var input0 = "abbbbc";
    var results = ["abbb"];
    shouldBe("regex283.exec(input0);", "results");

    var regex284 = /ab{0,}bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex284.exec(input0);", "results");

    var regex285 = /ab+bc/;
    var input0 = "abbc";
    var results = ["abbc"];
    shouldBe("regex285.exec(input0);", "results");
    // Failers
    var input1 = "abc";
    var results = null;
    shouldBe("regex285.exec(input1);", "results");
    var input2 = "abq";
    var results = null;
    shouldBe("regex285.exec(input2);", "results");

    var regex286 = /ab+bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex286.exec(input0);", "results");

    var regex287 = /ab{1,}bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex287.exec(input0);", "results");

    var regex288 = /ab{1,3}bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex288.exec(input0);", "results");

    var regex289 = /ab{3,4}bc/;
    var input0 = "abbbbc";
    var results = ["abbbbc"];
    shouldBe("regex289.exec(input0);", "results");

    var regex290 = /ab{4,5}bc/;
    // Failers
    var input0 = "abq";
    var results = null;
    shouldBe("regex290.exec(input0);", "results");
    var input1 = "abbbbc";
    var results = null;
    shouldBe("regex290.exec(input1);", "results");

    var regex291 = /ab?bc/;
    var input0 = "abbc";
    var results = ["abbc"];
    shouldBe("regex291.exec(input0);", "results");
    var input1 = "abc";
    var results = ["abc"];
    shouldBe("regex291.exec(input1);", "results");

    var regex292 = /ab{0,1}bc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex292.exec(input0);", "results");

    var regex293 = /ab?c/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex293.exec(input0);", "results");

    var regex294 = /ab{0,1}c/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex294.exec(input0);", "results");

    var regex295 = /^abc/;
    var input0 = "abcc";
    var results = ["abc"];
    shouldBe("regex295.exec(input0);", "results");

    var regex296 = /abc$/;
    var input0 = "aabc";
    var results = ["abc"];
    shouldBe("regex296.exec(input0);", "results");
    // Failers
    var input1 = "aabc";
    var results = ["abc"];
    shouldBe("regex296.exec(input1);", "results");
    var input2 = "aabcd";
    var results = null;
    shouldBe("regex296.exec(input2);", "results");

    var regex297 = /^/;
    var input0 = "abc";
    var results = [""];
    shouldBe("regex297.exec(input0);", "results");

    var regex298 = /$/;
    var input0 = "abc";
    var results = [""];
    shouldBe("regex298.exec(input0);", "results");

    var regex299 = /a.c/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex299.exec(input0);", "results");
    var input1 = "axc";
    var results = ["axc"];
    shouldBe("regex299.exec(input1);", "results");

    var regex300 = /a.*c/;
    var input0 = "axyzc";
    var results = ["axyzc"];
    shouldBe("regex300.exec(input0);", "results");

    var regex301 = /a[bc]d/;
    var input0 = "abd";
    var results = ["abd"];
    shouldBe("regex301.exec(input0);", "results");
    // Failers
    var input1 = "axyzd";
    var results = null;
    shouldBe("regex301.exec(input1);", "results");
    var input2 = "abc";
    var results = null;
    shouldBe("regex301.exec(input2);", "results");

    var regex302 = /a[b-d]e/;
    var input0 = "ace";
    var results = ["ace"];
    shouldBe("regex302.exec(input0);", "results");

    var regex303 = /a[b-d]/;
    var input0 = "aac";
    var results = ["ac"];
    shouldBe("regex303.exec(input0);", "results");

    var regex304 = /a[-b]/;
    var input0 = "a-";
    var results = ["a-"];
    shouldBe("regex304.exec(input0);", "results");

    var regex305 = /a[b-]/;
    var input0 = "a-";
    var results = ["a-"];
    shouldBe("regex305.exec(input0);", "results");

    var regex306 = /a]/;
    var input0 = "a]";
    var results = ["a]"];
    shouldBe("regex306.exec(input0);", "results");

    var regex307 = /a[\]]b/;
    var input0 = "a]b";
    var results = ["a]b"];
    shouldBe("regex307.exec(input0);", "results");

    var regex308 = /a[^bc]d/;
    var input0 = "aed";
    var results = ["aed"];
    shouldBe("regex308.exec(input0);", "results");
    // Failers
    var input1 = "abd";
    var results = null;
    shouldBe("regex308.exec(input1);", "results");
    var input2 = "abd";
    var results = null;
    shouldBe("regex308.exec(input2);", "results");

    var regex309 = /a[^-b]c/;
    var input0 = "adc";
    var results = ["adc"];
    shouldBe("regex309.exec(input0);", "results");

    var regex310 = /a[^\]b]c/;
    var input0 = "adc";
    var results = ["adc"];
    shouldBe("regex310.exec(input0);", "results");
    var input1 = "a-c";
    var results = ["a-c"];
    shouldBe("regex310.exec(input1);", "results");
    // Failers
    var input2 = "a]c";
    var results = null;
    shouldBe("regex310.exec(input2);", "results");

    var regex311 = /\ba\b/;
    var input0 = "a-";
    var results = ["a"];
    shouldBe("regex311.exec(input0);", "results");
    var input1 = "-a";
    var results = ["a"];
    shouldBe("regex311.exec(input1);", "results");
    var input2 = "-a-";
    var results = ["a"];
    shouldBe("regex311.exec(input2);", "results");

    var regex312 = /\by\b/;
    // Failers
    var input0 = "xy";
    var results = null;
    shouldBe("regex312.exec(input0);", "results");
    var input1 = "yz";
    var results = null;
    shouldBe("regex312.exec(input1);", "results");
    var input2 = "xyz";
    var results = null;
    shouldBe("regex312.exec(input2);", "results");

    var regex313 = /\Ba\B/;
    // Failers
    var input0 = "a-";
    var results = null;
    shouldBe("regex313.exec(input0);", "results");
    var input1 = "-a";
    var results = null;
    shouldBe("regex313.exec(input1);", "results");
    var input2 = "-a-";
    var results = null;
    shouldBe("regex313.exec(input2);", "results");

    var regex314 = /\By\b/;
    var input0 = "xy";
    var results = ["y"];
    shouldBe("regex314.exec(input0);", "results");

    var regex315 = /\by\B/;
    var input0 = "yz";
    var results = ["y"];
    shouldBe("regex315.exec(input0);", "results");

    var regex316 = /\By\B/;
    var input0 = "xyz";
    var results = ["y"];
    shouldBe("regex316.exec(input0);", "results");

    var regex317 = /\w/;
    var input0 = "a";
    var results = ["a"];
    shouldBe("regex317.exec(input0);", "results");

    var regex318 = /\W/;
    var input0 = "-";
    var results = ["-"];
    shouldBe("regex318.exec(input0);", "results");
    // Failers
    var input1 = "-";
    var results = ["-"];
    shouldBe("regex318.exec(input1);", "results");
    var input2 = "a";
    var results = null;
    shouldBe("regex318.exec(input2);", "results");

    var regex319 = /a\sb/;
    var input0 = "a b";
    var results = ["a b"];
    shouldBe("regex319.exec(input0);", "results");

    var regex320 = /a\Sb/;
    var input0 = "a-b";
    var results = ["a-b"];
    shouldBe("regex320.exec(input0);", "results");
    // Failers
    var input1 = "a-b";
    var results = ["a-b"];
    shouldBe("regex320.exec(input1);", "results");
    var input2 = "a b";
    var results = null;
    shouldBe("regex320.exec(input2);", "results");

    var regex321 = /\d/;
    var input0 = "1";
    var results = ["1"];
    shouldBe("regex321.exec(input0);", "results");

    var regex322 = /\D/;
    var input0 = "-";
    var results = ["-"];
    shouldBe("regex322.exec(input0);", "results");
    // Failers
    var input1 = "-";
    var results = ["-"];
    shouldBe("regex322.exec(input1);", "results");
    var input2 = "1";
    var results = null;
    shouldBe("regex322.exec(input2);", "results");

    var regex323 = /[\w]/;
    var input0 = "a";
    var results = ["a"];
    shouldBe("regex323.exec(input0);", "results");

    var regex324 = /[\W]/;
    var input0 = "-";
    var results = ["-"];
    shouldBe("regex324.exec(input0);", "results");
    // Failers
    var input1 = "-";
    var results = ["-"];
    shouldBe("regex324.exec(input1);", "results");
    var input2 = "a";
    var results = null;
    shouldBe("regex324.exec(input2);", "results");

    var regex325 = /a[\s]b/;
    var input0 = "a b";
    var results = ["a b"];
    shouldBe("regex325.exec(input0);", "results");

    var regex326 = /a[\S]b/;
    var input0 = "a-b";
    var results = ["a-b"];
    shouldBe("regex326.exec(input0);", "results");
    // Failers
    var input1 = "a-b";
    var results = ["a-b"];
    shouldBe("regex326.exec(input1);", "results");
    var input2 = "a b";
    var results = null;
    shouldBe("regex326.exec(input2);", "results");

    var regex327 = /[\d]/;
    var input0 = "1";
    var results = ["1"];
    shouldBe("regex327.exec(input0);", "results");

    var regex328 = /[\D]/;
    var input0 = "-";
    var results = ["-"];
    shouldBe("regex328.exec(input0);", "results");
    // Failers
    var input1 = "-";
    var results = ["-"];
    shouldBe("regex328.exec(input1);", "results");
    var input2 = "1";
    var results = null;
    shouldBe("regex328.exec(input2);", "results");

    var regex329 = /ab|cd/;
    var input0 = "abc";
    var results = ["ab"];
    shouldBe("regex329.exec(input0);", "results");
    var input1 = "abcd";
    var results = ["ab"];
    shouldBe("regex329.exec(input1);", "results");

    var regex330 = /()ef/;
    var input0 = "def";
    var results = ["ef", ""];
    shouldBe("regex330.exec(input0);", "results");

    var regex331 = /a\(b/;
    var input0 = "a(b";
    var results = ["a(b"];
    shouldBe("regex331.exec(input0);", "results");

    var regex332 = /a\(*b/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex332.exec(input0);", "results");
    var input1 = "a((b";
    var results = ["a((b"];
    shouldBe("regex332.exec(input1);", "results");

    var regex333 = /a\\b/;
    var input0 = "a\b";
    var results = null;
    shouldBe("regex333.exec(input0);", "results");

    var regex334 = /((a))/;
    var input0 = "abc";
    var results = ["a", "a", "a"];
    shouldBe("regex334.exec(input0);", "results");

    var regex335 = /(a)b(c)/;
    var input0 = "abc";
    var results = ["abc", "a", "c"];
    shouldBe("regex335.exec(input0);", "results");

    var regex336 = /a+b+c/;
    var input0 = "aabbabc";
    var results = ["abc"];
    shouldBe("regex336.exec(input0);", "results");

    var regex337 = /a{1,}b{1,}c/;
    var input0 = "aabbabc";
    var results = ["abc"];
    shouldBe("regex337.exec(input0);", "results");

    var regex338 = /a.+?c/;
    var input0 = "abcabc";
    var results = ["abc"];
    shouldBe("regex338.exec(input0);", "results");

    var regex339 = /(a+|b)*/;
    var input0 = "ab";
    var results = ["ab", "b"];
    shouldBe("regex339.exec(input0);", "results");

    var regex340 = /(a+|b){0,}/;
    var input0 = "ab";
    var results = ["ab", "b"];
    shouldBe("regex340.exec(input0);", "results");

    var regex341 = /(a+|b)+/;
    var input0 = "ab";
    var results = ["ab", "b"];
    shouldBe("regex341.exec(input0);", "results");

    var regex342 = /(a+|b){1,}/;
    var input0 = "ab";
    var results = ["ab", "b"];
    shouldBe("regex342.exec(input0);", "results");

    var regex343 = /(a+|b)?/;
    var input0 = "ab";
    var results = ["a", "a"];
    shouldBe("regex343.exec(input0);", "results");

    var regex344 = /(a+|b){0,1}/;
    var input0 = "ab";
    var results = ["a", "a"];
    shouldBe("regex344.exec(input0);", "results");

    var regex345 = /[^ab]*/;
    var input0 = "cde";
    var results = ["cde"];
    shouldBe("regex345.exec(input0);", "results");

    var regex346 = /abc/;
    // Failers
    var input0 = "b";
    var results = null;
    shouldBe("regex346.exec(input0);", "results");

    var regex347 = /([abc])*d/;
    var input0 = "abbbcd";
    var results = ["abbbcd", "c"];
    shouldBe("regex347.exec(input0);", "results");

    var regex348 = /([abc])*bcd/;
    var input0 = "abcd";
    var results = ["abcd", "a"];
    shouldBe("regex348.exec(input0);", "results");

    var regex349 = /a|b|c|d|e/;
    var input0 = "e";
    var results = ["e"];
    shouldBe("regex349.exec(input0);", "results");

    var regex350 = /(a|b|c|d|e)f/;
    var input0 = "ef";
    var results = ["ef", "e"];
    shouldBe("regex350.exec(input0);", "results");

    var regex351 = /abcd*efg/;
    var input0 = "abcdefg";
    var results = ["abcdefg"];
    shouldBe("regex351.exec(input0);", "results");

    var regex352 = /ab*/;
    var input0 = "xabyabbbz";
    var results = ["ab"];
    shouldBe("regex352.exec(input0);", "results");
    var input1 = "xayabbbz";
    var results = ["a"];
    shouldBe("regex352.exec(input1);", "results");

    var regex353 = /(ab|cd)e/;
    var input0 = "abcde";
    var results = ["cde", "cd"];
    shouldBe("regex353.exec(input0);", "results");

    var regex354 = /[abhgefdc]ij/;
    var input0 = "hij";
    var results = ["hij"];
    shouldBe("regex354.exec(input0);", "results");

    var regex355 = /(abc|)ef/;
    var input0 = "abcdef";
    var results = ["ef", ""];
    shouldBe("regex355.exec(input0);", "results");

    var regex356 = /(a|b)c*d/;
    var input0 = "abcd";
    var results = ["bcd", "b"];
    shouldBe("regex356.exec(input0);", "results");

    var regex357 = /(ab|ab*)bc/;
    var input0 = "abc";
    var results = ["abc", "a"];
    shouldBe("regex357.exec(input0);", "results");

    var regex358 = /a([bc]*)c*/;
    var input0 = "abc";
    var results = ["abc", "bc"];
    shouldBe("regex358.exec(input0);", "results");

    var regex359 = /a([bc]*)(c*d)/;
    var input0 = "abcd";
    var results = ["abcd", "bc", "d"];
    shouldBe("regex359.exec(input0);", "results");

    var regex360 = /a([bc]+)(c*d)/;
    var input0 = "abcd";
    var results = ["abcd", "bc", "d"];
    shouldBe("regex360.exec(input0);", "results");

    var regex361 = /a([bc]*)(c+d)/;
    var input0 = "abcd";
    var results = ["abcd", "b", "cd"];
    shouldBe("regex361.exec(input0);", "results");

    var regex362 = /a[bcd]*dcdcde/;
    var input0 = "adcdcde";
    var results = ["adcdcde"];
    shouldBe("regex362.exec(input0);", "results");

    var regex363 = /a[bcd]+dcdcde/;
    // Failers
    var input0 = "abcde";
    var results = null;
    shouldBe("regex363.exec(input0);", "results");
    var input1 = "adcdcde";
    var results = null;
    shouldBe("regex363.exec(input1);", "results");

    var regex364 = /(ab|a)b*c/;
    var input0 = "abc";
    var results = ["abc", "ab"];
    shouldBe("regex364.exec(input0);", "results");

    var regex365 = /((a)(b)c)(d)/;
    var input0 = "abcd";
    var results = ["abcd", "abc", "a", "b", "d"];
    shouldBe("regex365.exec(input0);", "results");

    var regex366 = /[a-zA-Z_][a-zA-Z0-9_]*/;
    var input0 = "alpha";
    var results = ["alpha"];
    shouldBe("regex366.exec(input0);", "results");

    var regex367 = /^a(bc+|b[eh])g|.h$/;
    var input0 = "abh";
    var results = ["bh", undefined];
    shouldBe("regex367.exec(input0);", "results");

    var regex368 = /(bc+d$|ef*g.|h?i(j|k))/;
    var input0 = "effgz";
    var results = ["effgz", "effgz", undefined];
    shouldBe("regex368.exec(input0);", "results");
    var input1 = "ij";
    var results = ["ij", "ij", "j"];
    shouldBe("regex368.exec(input1);", "results");
    var input2 = "reffgz";
    var results = ["effgz", "effgz", undefined];
    shouldBe("regex368.exec(input2);", "results");
    // Failers
    var input3 = "effg";
    var results = null;
    shouldBe("regex368.exec(input3);", "results");
    var input4 = "bcdd";
    var results = null;
    shouldBe("regex368.exec(input4);", "results");

    var regex369 = /((((((((((a))))))))))/;
    var input0 = "a";
    var results = ["a", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a"];
    shouldBe("regex369.exec(input0);", "results");

    var regex370 = /((((((((((a))))))))))\10/;
    var input0 = "aa";
    var results = ["aa", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a"];
    shouldBe("regex370.exec(input0);", "results");

    var regex371 = /(((((((((a)))))))))/;
    var input0 = "a";
    var results = ["a", "a", "a", "a", "a", "a", "a", "a", "a", "a"];
    shouldBe("regex371.exec(input0);", "results");

    var regex372 = /multiple words of text/;
    // Failers
    var input0 = "aa";
    var results = null;
    shouldBe("regex372.exec(input0);", "results");
    var input1 = "uh-uh";
    var results = null;
    shouldBe("regex372.exec(input1);", "results");

    var regex373 = /multiple words/;
    var input0 = "multiple words, yeah";
    var results = ["multiple words"];
    shouldBe("regex373.exec(input0);", "results");

    var regex374 = /(.*)c(.*)/;
    var input0 = "abcde";
    var results = ["abcde", "ab", "de"];
    shouldBe("regex374.exec(input0);", "results");

    var regex375 = /\((.*), (.*)\)/;
    var input0 = "(a, b)";
    var results = ["(a, b)", "a", "b"];
    shouldBe("regex375.exec(input0);", "results");

    var regex376 = /abcd/;
    var input0 = "abcd";
    var results = ["abcd"];
    shouldBe("regex376.exec(input0);", "results");

    var regex377 = /a(bc)d/;
    var input0 = "abcd";
    var results = ["abcd", "bc"];
    shouldBe("regex377.exec(input0);", "results");

    var regex378 = /a[-]?c/;
    var input0 = "ac";
    var results = ["ac"];
    shouldBe("regex378.exec(input0);", "results");

    var regex379 = /(abc)\1/;
    var input0 = "abcabc";
    var results = ["abcabc", "abc"];
    shouldBe("regex379.exec(input0);", "results");

    var regex380 = /([a-c]*)\1/;
    var input0 = "abcabc";
    var results = ["abcabc", "abc"];
    shouldBe("regex380.exec(input0);", "results");

    var regex381 = /(a)|\1/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex381.exec(input0);", "results");
    var input1 = "ab";
    var results = ["a", "a"];
    shouldBe("regex381.exec(input1);", "results");
    var input2 = "x";
    var results = ["", undefined];
    shouldBe("regex381.exec(input2);", "results");

    var regex382 = /(([a-c])b*?\2)*/;
    var input0 = "ababbbcbc";
    var results = ["ababb", "bb", "b"];
    shouldBe("regex382.exec(input0);", "results");

    var regex383 = /(([a-c])b*?\2){3}/;
    var input0 = "ababbbcbc";
    var results = ["ababbbcbc", "cbc", "c"];
    shouldBe("regex383.exec(input0);", "results");

    var regex384 = /((\3|b)\2(a)x)+/;
    var input0 = "aaaxabaxbaaxbbax";
    var results = ["ax", "ax", "", "a"];
    shouldBe("regex384.exec(input0);", "results");

    var regex385 = /((\3|b)\2(a)){2,}/;
    var input0 = "bbaababbabaaaaabbaaaabba";
    var results = ["bbaa", "a", "", "a"];
    shouldBe("regex385.exec(input0);", "results");

    var regex386 = /abc/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex386.exec(input0);", "results");
    var input1 = "XABCY";
    var results = ["ABC"];
    shouldBe("regex386.exec(input1);", "results");
    var input2 = "ABABC";
    var results = ["ABC"];
    shouldBe("regex386.exec(input2);", "results");
    // Failers
    var input3 = "aaxabxbaxbbx";
    var results = null;
    shouldBe("regex386.exec(input3);", "results");
    var input4 = "XBC";
    var results = null;
    shouldBe("regex386.exec(input4);", "results");
    var input5 = "AXC";
    var results = null;
    shouldBe("regex386.exec(input5);", "results");
    var input6 = "ABX";
    var results = null;
    shouldBe("regex386.exec(input6);", "results");

    var regex387 = /ab*c/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex387.exec(input0);", "results");

    var regex388 = /ab*bc/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex388.exec(input0);", "results");
    var input1 = "ABBC";
    var results = ["ABBC"];
    shouldBe("regex388.exec(input1);", "results");

    var regex389 = /ab*?bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex389.exec(input0);", "results");

    var regex390 = /ab{0,}?bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex390.exec(input0);", "results");

    var regex391 = /ab+?bc/i;
    var input0 = "ABBC";
    var results = ["ABBC"];
    shouldBe("regex391.exec(input0);", "results");

    var regex392 = /ab+bc/i;
    // Failers
    var input0 = "ABC";
    var results = null;
    shouldBe("regex392.exec(input0);", "results");
    var input1 = "ABQ";
    var results = null;
    shouldBe("regex392.exec(input1);", "results");

    var regex393 = /ab+bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex393.exec(input0);", "results");

    var regex394 = /ab{1,}?bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex394.exec(input0);", "results");

    var regex395 = /ab{1,3}?bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex395.exec(input0);", "results");

    var regex396 = /ab{3,4}?bc/i;
    var input0 = "ABBBBC";
    var results = ["ABBBBC"];
    shouldBe("regex396.exec(input0);", "results");

    var regex397 = /ab{4,5}?bc/i;
    // Failers
    var input0 = "ABQ";
    var results = null;
    shouldBe("regex397.exec(input0);", "results");
    var input1 = "ABBBBC";
    var results = null;
    shouldBe("regex397.exec(input1);", "results");

    var regex398 = /ab??bc/i;
    var input0 = "ABBC";
    var results = ["ABBC"];
    shouldBe("regex398.exec(input0);", "results");
    var input1 = "ABC";
    var results = ["ABC"];
    shouldBe("regex398.exec(input1);", "results");

    var regex399 = /ab{0,1}?bc/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex399.exec(input0);", "results");

    var regex400 = /ab??c/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex400.exec(input0);", "results");

    var regex401 = /ab{0,1}?c/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex401.exec(input0);", "results");

    var regex402 = /^abc$/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex402.exec(input0);", "results");
    // Failers
    var input1 = "ABBBBC";
    var results = null;
    shouldBe("regex402.exec(input1);", "results");
    var input2 = "ABCC";
    var results = null;
    shouldBe("regex402.exec(input2);", "results");

    var regex403 = /^abc/i;
    var input0 = "ABCC";
    var results = ["ABC"];
    shouldBe("regex403.exec(input0);", "results");

    var regex404 = /abc$/i;
    var input0 = "AABC";
    var results = ["ABC"];
    shouldBe("regex404.exec(input0);", "results");

    var regex405 = /^/i;
    var input0 = "ABC";
    var results = [""];
    shouldBe("regex405.exec(input0);", "results");

    var regex406 = /$/i;
    var input0 = "ABC";
    var results = [""];
    shouldBe("regex406.exec(input0);", "results");

    var regex407 = /a.c/i;
    var input0 = "ABC";
    var results = ["ABC"];
    shouldBe("regex407.exec(input0);", "results");
    var input1 = "AXC";
    var results = ["AXC"];
    shouldBe("regex407.exec(input1);", "results");

    var regex408 = /a.*?c/i;
    var input0 = "AXYZC";
    var results = ["AXYZC"];
    shouldBe("regex408.exec(input0);", "results");

    var regex409 = /a.*c/i;
    // Failers
    var input0 = "AABC";
    var results = ["AABC"];
    shouldBe("regex409.exec(input0);", "results");
    var input1 = "AXYZD";
    var results = null;
    shouldBe("regex409.exec(input1);", "results");

    var regex410 = /a[bc]d/i;
    var input0 = "ABD";
    var results = ["ABD"];
    shouldBe("regex410.exec(input0);", "results");

    var regex411 = /a[b-d]e/i;
    var input0 = "ACE";
    var results = ["ACE"];
    shouldBe("regex411.exec(input0);", "results");
    // Failers
    var input1 = "ABC";
    var results = null;
    shouldBe("regex411.exec(input1);", "results");
    var input2 = "ABD";
    var results = null;
    shouldBe("regex411.exec(input2);", "results");

    var regex412 = /a[b-d]/i;
    var input0 = "AAC";
    var results = ["AC"];
    shouldBe("regex412.exec(input0);", "results");

    var regex413 = /a[-b]/i;
    var input0 = "A-";
    var results = ["A-"];
    shouldBe("regex413.exec(input0);", "results");

    var regex414 = /a[b-]/i;
    var input0 = "A-";
    var results = ["A-"];
    shouldBe("regex414.exec(input0);", "results");

    var regex415 = /a]/i;
    var input0 = "A]";
    var results = ["A]"];
    shouldBe("regex415.exec(input0);", "results");

    var regex416 = /a[\]]b/i;
    var input0 = "A]B";
    var results = ["A]B"];
    shouldBe("regex416.exec(input0);", "results");

    var regex417 = /a[^bc]d/i;
    var input0 = "AED";
    var results = ["AED"];
    shouldBe("regex417.exec(input0);", "results");

    var regex418 = /a[^-b]c/i;
    var input0 = "ADC";
    var results = ["ADC"];
    shouldBe("regex418.exec(input0);", "results");
    // Failers
    var input1 = "ABD";
    var results = null;
    shouldBe("regex418.exec(input1);", "results");
    var input2 = "A-C";
    var results = null;
    shouldBe("regex418.exec(input2);", "results");

    var regex419 = /a[^\]b]c/i;
    var input0 = "ADC";
    var results = ["ADC"];
    shouldBe("regex419.exec(input0);", "results");

    var regex420 = /ab|cd/i;
    var input0 = "ABC";
    var results = ["AB"];
    shouldBe("regex420.exec(input0);", "results");
    var input1 = "ABCD";
    var results = ["AB"];
    shouldBe("regex420.exec(input1);", "results");

    var regex421 = /()ef/i;
    var input0 = "DEF";
    var results = ["EF", ""];
    shouldBe("regex421.exec(input0);", "results");

    var regex422 = /$b/i;
    // Failers
    var input0 = "A]C";
    var results = null;
    shouldBe("regex422.exec(input0);", "results");
    var input1 = "B";
    var results = null;
    shouldBe("regex422.exec(input1);", "results");

    var regex423 = /a\(b/i;
    var input0 = "A(B";
    var results = ["A(B"];
    shouldBe("regex423.exec(input0);", "results");

    var regex424 = /a\(*b/i;
    var input0 = "AB";
    var results = ["AB"];
    shouldBe("regex424.exec(input0);", "results");
    var input1 = "A((B";
    var results = ["A((B"];
    shouldBe("regex424.exec(input1);", "results");

    var regex425 = /a\\b/i;
    var input0 = "A\B";
    var results = null;
    shouldBe("regex425.exec(input0);", "results");

    var regex426 = /((a))/i;
    var input0 = "ABC";
    var results = ["A", "A", "A"];
    shouldBe("regex426.exec(input0);", "results");

    var regex427 = /(a)b(c)/i;
    var input0 = "ABC";
    var results = ["ABC", "A", "C"];
    shouldBe("regex427.exec(input0);", "results");

    var regex428 = /a+b+c/i;
    var input0 = "AABBABC";
    var results = ["ABC"];
    shouldBe("regex428.exec(input0);", "results");

    var regex429 = /a{1,}b{1,}c/i;
    var input0 = "AABBABC";
    var results = ["ABC"];
    shouldBe("regex429.exec(input0);", "results");

    var regex430 = /a.+?c/i;
    var input0 = "ABCABC";
    var results = ["ABC"];
    shouldBe("regex430.exec(input0);", "results");

    var regex431 = /a.*?c/i;
    var input0 = "ABCABC";
    var results = ["ABC"];
    shouldBe("regex431.exec(input0);", "results");

    var regex432 = /a.{0,5}?c/i;
    var input0 = "ABCABC";
    var results = ["ABC"];
    shouldBe("regex432.exec(input0);", "results");

    var regex433 = /(a+|b)*/i;
    var input0 = "AB";
    var results = ["AB", "B"];
    shouldBe("regex433.exec(input0);", "results");

    var regex434 = /(a+|b){0,}/i;
    var input0 = "AB";
    var results = ["AB", "B"];
    shouldBe("regex434.exec(input0);", "results");

    var regex435 = /(a+|b)+/i;
    var input0 = "AB";
    var results = ["AB", "B"];
    shouldBe("regex435.exec(input0);", "results");

    var regex436 = /(a+|b){1,}/i;
    var input0 = "AB";
    var results = ["AB", "B"];
    shouldBe("regex436.exec(input0);", "results");

    var regex437 = /(a+|b)?/i;
    var input0 = "AB";
    var results = ["A", "A"];
    shouldBe("regex437.exec(input0);", "results");

    var regex438 = /(a+|b){0,1}/i;
    var input0 = "AB";
    var results = ["A", "A"];
    shouldBe("regex438.exec(input0);", "results");

    var regex439 = /(a+|b){0,1}?/i;
    var input0 = "AB";
    var results = ["", undefined];
    shouldBe("regex439.exec(input0);", "results");

    var regex440 = /[^ab]*/i;
    var input0 = "CDE";
    var results = ["CDE"];
    shouldBe("regex440.exec(input0);", "results");

    var regex441 = /([abc])*d/i;
    var input0 = "ABBBCD";
    var results = ["ABBBCD", "C"];
    shouldBe("regex441.exec(input0);", "results");

    var regex442 = /([abc])*bcd/i;
    var input0 = "ABCD";
    var results = ["ABCD", "A"];
    shouldBe("regex442.exec(input0);", "results");

    var regex443 = /a|b|c|d|e/i;
    var input0 = "E";
    var results = ["E"];
    shouldBe("regex443.exec(input0);", "results");

    var regex444 = /(a|b|c|d|e)f/i;
    var input0 = "EF";
    var results = ["EF", "E"];
    shouldBe("regex444.exec(input0);", "results");

    var regex445 = /abcd*efg/i;
    var input0 = "ABCDEFG";
    var results = ["ABCDEFG"];
    shouldBe("regex445.exec(input0);", "results");

    var regex446 = /ab*/i;
    var input0 = "XABYABBBZ";
    var results = ["AB"];
    shouldBe("regex446.exec(input0);", "results");
    var input1 = "XAYABBBZ";
    var results = ["A"];
    shouldBe("regex446.exec(input1);", "results");

    var regex447 = /(ab|cd)e/i;
    var input0 = "ABCDE";
    var results = ["CDE", "CD"];
    shouldBe("regex447.exec(input0);", "results");

    var regex448 = /[abhgefdc]ij/i;
    var input0 = "HIJ";
    var results = ["HIJ"];
    shouldBe("regex448.exec(input0);", "results");

    var regex449 = /^(ab|cd)e/i;
    var input0 = "ABCDE";
    var results = null;
    shouldBe("regex449.exec(input0);", "results");

    var regex450 = /(abc|)ef/i;
    var input0 = "ABCDEF";
    var results = ["EF", ""];
    shouldBe("regex450.exec(input0);", "results");

    var regex451 = /(a|b)c*d/i;
    var input0 = "ABCD";
    var results = ["BCD", "B"];
    shouldBe("regex451.exec(input0);", "results");

    var regex452 = /(ab|ab*)bc/i;
    var input0 = "ABC";
    var results = ["ABC", "A"];
    shouldBe("regex452.exec(input0);", "results");

    var regex453 = /a([bc]*)c*/i;
    var input0 = "ABC";
    var results = ["ABC", "BC"];
    shouldBe("regex453.exec(input0);", "results");

    var regex454 = /a([bc]*)(c*d)/i;
    var input0 = "ABCD";
    var results = ["ABCD", "BC", "D"];
    shouldBe("regex454.exec(input0);", "results");

    var regex455 = /a([bc]+)(c*d)/i;
    var input0 = "ABCD";
    var results = ["ABCD", "BC", "D"];
    shouldBe("regex455.exec(input0);", "results");

    var regex456 = /a([bc]*)(c+d)/i;
    var input0 = "ABCD";
    var results = ["ABCD", "B", "CD"];
    shouldBe("regex456.exec(input0);", "results");

    var regex457 = /a[bcd]*dcdcde/i;
    var input0 = "ADCDCDE";
    var results = ["ADCDCDE"];
    shouldBe("regex457.exec(input0);", "results");

    var regex458 = /(ab|a)b*c/i;
    var input0 = "ABC";
    var results = ["ABC", "AB"];
    shouldBe("regex458.exec(input0);", "results");

    var regex459 = /((a)(b)c)(d)/i;
    var input0 = "ABCD";
    var results = ["ABCD", "ABC", "A", "B", "D"];
    shouldBe("regex459.exec(input0);", "results");

    var regex460 = /[a-zA-Z_][a-zA-Z0-9_]*/i;
    var input0 = "ALPHA";
    var results = ["ALPHA"];
    shouldBe("regex460.exec(input0);", "results");

    var regex461 = /^a(bc+|b[eh])g|.h$/i;
    var input0 = "ABH";
    var results = ["BH", undefined];
    shouldBe("regex461.exec(input0);", "results");

    var regex462 = /(bc+d$|ef*g.|h?i(j|k))/i;
    var input0 = "EFFGZ";
    var results = ["EFFGZ", "EFFGZ", undefined];
    shouldBe("regex462.exec(input0);", "results");
    var input1 = "IJ";
    var results = ["IJ", "IJ", "J"];
    shouldBe("regex462.exec(input1);", "results");
    var input2 = "REFFGZ";
    var results = ["EFFGZ", "EFFGZ", undefined];
    shouldBe("regex462.exec(input2);", "results");
    // Failers
    var input3 = "ADCDCDE";
    var results = null;
    shouldBe("regex462.exec(input3);", "results");
    var input4 = "EFFG";
    var results = null;
    shouldBe("regex462.exec(input4);", "results");
    var input5 = "BCDD";
    var results = null;
    shouldBe("regex462.exec(input5);", "results");

    var regex463 = /((((((((((a))))))))))/i;
    var input0 = "A";
    var results = ["A", "A", "A", "A", "A", "A", "A", "A", "A", "A", "A"];
    shouldBe("regex463.exec(input0);", "results");

    var regex464 = /((((((((((a))))))))))\10/i;
    var input0 = "AA";
    var results = ["AA", "A", "A", "A", "A", "A", "A", "A", "A", "A", "A"];
    shouldBe("regex464.exec(input0);", "results");

    var regex465 = /(((((((((a)))))))))/i;
    var input0 = "A";
    var results = ["A", "A", "A", "A", "A", "A", "A", "A", "A", "A"];
    shouldBe("regex465.exec(input0);", "results");

    var regex466 = /(?:(?:(?:(?:(?:(?:(?:(?:(?:(a))))))))))/i;
    var input0 = "A";
    var results = ["A", "A"];
    shouldBe("regex466.exec(input0);", "results");

    var regex467 = /(?:(?:(?:(?:(?:(?:(?:(?:(?:(a|b|c))))))))))/i;
    var input0 = "C";
    var results = ["C", "C"];
    shouldBe("regex467.exec(input0);", "results");

    var regex468 = /multiple words of text/i;
    // Failers
    var input0 = "AA";
    var results = null;
    shouldBe("regex468.exec(input0);", "results");
    var input1 = "UH-UH";
    var results = null;
    shouldBe("regex468.exec(input1);", "results");

    var regex469 = /multiple words/i;
    var input0 = "MULTIPLE WORDS, YEAH";
    var results = ["MULTIPLE WORDS"];
    shouldBe("regex469.exec(input0);", "results");

    var regex470 = /(.*)c(.*)/i;
    var input0 = "ABCDE";
    var results = ["ABCDE", "AB", "DE"];
    shouldBe("regex470.exec(input0);", "results");

    var regex471 = /\((.*), (.*)\)/i;
    var input0 = "(A, B)";
    var results = ["(A, B)", "A", "B"];
    shouldBe("regex471.exec(input0);", "results");

    var regex472 = /abcd/i;
    var input0 = "ABCD";
    var results = ["ABCD"];
    shouldBe("regex472.exec(input0);", "results");

    var regex473 = /a(bc)d/i;
    var input0 = "ABCD";
    var results = ["ABCD", "BC"];
    shouldBe("regex473.exec(input0);", "results");

    var regex474 = /a[-]?c/i;
    var input0 = "AC";
    var results = ["AC"];
    shouldBe("regex474.exec(input0);", "results");

    var regex475 = /(abc)\1/i;
    var input0 = "ABCABC";
    var results = ["ABCABC", "ABC"];
    shouldBe("regex475.exec(input0);", "results");

    var regex476 = /([a-c]*)\1/i;
    var input0 = "ABCABC";
    var results = ["ABCABC", "ABC"];
    shouldBe("regex476.exec(input0);", "results");

    var regex477 = /a(?!b)./;
    var input0 = "abad";
    var results = ["ad"];
    shouldBe("regex477.exec(input0);", "results");

    var regex478 = /a(?=d)./;
    var input0 = "abad";
    var results = ["ad"];
    shouldBe("regex478.exec(input0);", "results");

    var regex479 = /a(?=c|d)./;
    var input0 = "abad";
    var results = ["ad"];
    shouldBe("regex479.exec(input0);", "results");

    var regex480 = /a(?:b|c|d)(.)/;
    var input0 = "ace";
    var results = ["ace", "e"];
    shouldBe("regex480.exec(input0);", "results");

    var regex481 = /a(?:b|c|d)*(.)/;
    var input0 = "ace";
    var results = ["ace", "e"];
    shouldBe("regex481.exec(input0);", "results");

    var regex482 = /a(?:b|c|d)+?(.)/;
    var input0 = "ace";
    var results = ["ace", "e"];
    shouldBe("regex482.exec(input0);", "results");
    var input1 = "acdbcdbe";
    var results = ["acd", "d"];
    shouldBe("regex482.exec(input1);", "results");

    var regex483 = /a(?:b|c|d)+(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdbe", "e"];
    shouldBe("regex483.exec(input0);", "results");

    var regex484 = /a(?:b|c|d){2}(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdb", "b"];
    shouldBe("regex484.exec(input0);", "results");

    var regex485 = /a(?:b|c|d){4,5}(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdb", "b"];
    shouldBe("regex485.exec(input0);", "results");

    var regex486 = /a(?:b|c|d){4,5}?(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcd", "d"];
    shouldBe("regex486.exec(input0);", "results");

    var regex487 = /((foo)|(bar))*/;
    var input0 = "foobar";
    var results = ["foobar", "bar", undefined, "bar"];
    shouldBe("regex487.exec(input0);", "results");

    var regex488 = /a(?:b|c|d){6,7}(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdbe", "e"];
    shouldBe("regex488.exec(input0);", "results");

    var regex489 = /a(?:b|c|d){6,7}?(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdbe", "e"];
    shouldBe("regex489.exec(input0);", "results");

    var regex490 = /a(?:b|c|d){5,6}(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdbe", "e"];
    shouldBe("regex490.exec(input0);", "results");

    var regex491 = /a(?:b|c|d){5,6}?(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdb", "b"];
    shouldBe("regex491.exec(input0);", "results");

    var regex492 = /a(?:b|c|d){5,7}(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdbe", "e"];
    shouldBe("regex492.exec(input0);", "results");

    var regex493 = /a(?:b|c|d){5,7}?(.)/;
    var input0 = "acdbcdbe";
    var results = ["acdbcdb", "b"];
    shouldBe("regex493.exec(input0);", "results");

    var regex494 = /a(?:b|(c|e){1,2}?|d)+?(.)/;
    var input0 = "ace";
    var results = ["ace", "c", "e"];
    shouldBe("regex494.exec(input0);", "results");

    var regex495 = /^(.+)?B/;
    var input0 = "AB";
    var results = ["AB", "A"];
    shouldBe("regex495.exec(input0);", "results");

    var regex496 = /^([^a-z])|(\^)$/;
    var input0 = ".";
    var results = [".", ".", undefined];
    shouldBe("regex496.exec(input0);", "results");

    var regex497 = /^[<>]&/;
    var input0 = "<&OUT";
    var results = ["<&"];
    shouldBe("regex497.exec(input0);", "results");

    var regex498 = /^(a\1?){4}$/;
    var input0 = "aaaaaaaaaa";
    var results = null;
    shouldBe("regex498.exec(input0);", "results");
    // Failers
    var input1 = "AB";
    var results = null;
    shouldBe("regex498.exec(input1);", "results");
    var input2 = "aaaaaaaaa";
    var results = null;
    shouldBe("regex498.exec(input2);", "results");
    var input3 = "aaaaaaaaaaa";
    var results = null;
    shouldBe("regex498.exec(input3);", "results");

    var regex499 = /^(a(?:\1)){4}$/;
    var input0 = "aaaa";
    var results = ["aaaa", "a"];
    shouldBe("regex499.exec(input0);", "results");
    // Failers
    var input1 = "aaaaaaaaa";
    var results = null;
    shouldBe("regex499.exec(input1);", "results");
    var input2 = "aaaaaaaaaaa";
    var results = null;
    shouldBe("regex499.exec(input2);", "results");
    var input3 = "aaaaaaaaaa";
    var results = null;
    shouldBe("regex499.exec(input3);", "results");

    var regex500 = /(?:(f)(o)(o)|(b)(a)(r))*/;
    var input0 = "foobar";
    var results = ["foobar", undefined, undefined, undefined, "b", "a", "r"];
    shouldBe("regex500.exec(input0);", "results");

    var regex503 = /(?:..)*a/;
    var input0 = "aba";
    var results = ["aba"];
    shouldBe("regex503.exec(input0);", "results");

    var regex504 = /(?:..)*?a/;
    var input0 = "aba";
    var results = ["a"];
    shouldBe("regex504.exec(input0);", "results");

    var regex505 = /^(?:b|a(?=(.)))*\1/;
    var input0 = "abc";
    var results = ["ab", undefined];
    shouldBe("regex505.exec(input0);", "results");

    var regex506 = /^(){3,5}/;
    var input0 = "abc";
    var results = ["", ""];
    shouldBe("regex506.exec(input0);", "results");

    var regex507 = /^(a+)*ax/;
    var input0 = "aax";
    var results = ["aax", "a"];
    shouldBe("regex507.exec(input0);", "results");

    var regex508 = /^((a|b)+)*ax/;
    var input0 = "aax";
    var results = ["aax", "a", "a"];
    shouldBe("regex508.exec(input0);", "results");

    var regex509 = /^((a|bc)+)*ax/;
    var input0 = "aax";
    var results = ["aax", "a", "a"];
    shouldBe("regex509.exec(input0);", "results");

    var regex510 = /(a|x)*ab/;
    var input0 = "cab";
    var results = ["ab", undefined];
    shouldBe("regex510.exec(input0);", "results");

    var regex511 = /(a)*ab/;
    var input0 = "cab";
    var results = ["ab", undefined];
    shouldBe("regex511.exec(input0);", "results");

    var regex512 = /(?:a)b/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex512.exec(input0);", "results");

    var regex513 = /(a)b/;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex513.exec(input0);", "results");

    var regex514 = /(?:a)b/i;
    var input0 = "Ab";
    var results = ["Ab"];
    shouldBe("regex514.exec(input0);", "results");

    var regex515 = /(a)b/i;
    var input0 = "Ab";
    var results = ["Ab", "A"];
    shouldBe("regex515.exec(input0);", "results");

    var regex516 = /(?:[aA])b/;
    // Failers
    var input0 = "cb";
    var results = null;
    shouldBe("regex516.exec(input0);", "results");
    var input1 = "aB";
    var results = null;
    shouldBe("regex516.exec(input1);", "results");

    var regex517 = /(?:a)b/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex517.exec(input0);", "results");

    var regex518 = /((?:a))b/;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex518.exec(input0);", "results");

    var regex519 = /(?:a)b/i;
    var input0 = "Ab";
    var results = ["Ab"];
    shouldBe("regex519.exec(input0);", "results");

    var regex520 = /((?:a))b/i;
    var input0 = "Ab";
    var results = ["Ab", "A"];
    shouldBe("regex520.exec(input0);", "results");

    var regex521 = /(?:a)b/;
    // Failers
    var input0 = "aB";
    var results = null;
    shouldBe("regex521.exec(input0);", "results");
    var input1 = "aB";
    var results = null;
    shouldBe("regex521.exec(input1);", "results");

    var regex522 = /(?:a)b/i;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex522.exec(input0);", "results");

    var regex523 = /(a)b/i;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex523.exec(input0);", "results");

    var regex524 = /(?:a)b/i;
    var input0 = "aB";
    var results = ["aB"];
    shouldBe("regex524.exec(input0);", "results");

    var regex525 = /(a)b/i;
    var input0 = "aB";
    var results = ["aB", "a"];
    shouldBe("regex525.exec(input0);", "results");

    var regex526 = /(?:a)[bB]/;
    // Failers
    var input0 = "aB";
    var results = ["aB"];
    shouldBe("regex526.exec(input0);", "results");
    var input1 = "Ab";
    var results = null;
    shouldBe("regex526.exec(input1);", "results");

    var regex527 = /(?:a)b/i;
    var input0 = "aB";
    var results = ["aB"];
    shouldBe("regex527.exec(input0);", "results");

    var regex528 = /(a)b/i;
    var input0 = "aB";
    var results = ["aB", "a"];
    shouldBe("regex528.exec(input0);", "results");

    var regex529 = /(?:a)[bB]/;
    // Failers
    var input0 = "Ab";
    var results = null;
    shouldBe("regex529.exec(input0);", "results");
    var input1 = "AB";
    var results = null;
    shouldBe("regex529.exec(input1);", "results");

    var regex530 = /(?:a)b/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex530.exec(input0);", "results");

    var regex531 = /((?:a))b/i;
    var input0 = "ab";
    var results = ["ab", "a"];
    shouldBe("regex531.exec(input0);", "results");

    var regex532 = /(?:a)b/i;
    var input0 = "aB";
    var results = ["aB"];
    shouldBe("regex532.exec(input0);", "results");

    var regex533 = /((?:a))b/i;
    var input0 = "aB";
    var results = ["aB", "a"];
    shouldBe("regex533.exec(input0);", "results");

    var regex534 = /(?:a)[bB]/;
    // Failers
    var input0 = "AB";
    var results = null;
    shouldBe("regex534.exec(input0);", "results");
    var input1 = "Ab";
    var results = null;
    shouldBe("regex534.exec(input1);", "results");

    var regex535 = /(?:a)b/i;
    var input0 = "aB";
    var results = ["aB"];
    shouldBe("regex535.exec(input0);", "results");

    var regex536 = /((?:a))b/i;
    var input0 = "aB";
    var results = ["aB", "a"];
    shouldBe("regex536.exec(input0);", "results");

    var regex537 = /(?:a)[bB]/;
    // Failers
    var input0 = "Ab";
    var results = null;
    shouldBe("regex537.exec(input0);", "results");
    var input1 = "AB";
    var results = null;
    shouldBe("regex537.exec(input1);", "results");

    var regex538 = /((?:a.))b/i;
    // Failers
    var input0 = "AB";
    var results = null;
    shouldBe("regex538.exec(input0);", "results");
    var input1 = "a\nB";
    var results = null;
    shouldBe("regex538.exec(input1);", "results");

    var regex539 = /((?:a[\w\W]))b/i;
    var input0 = "a\nB";
    var results = ["a\x0aB", "a\x0a"];
    shouldBe("regex539.exec(input0);", "results");

    var regex540 = /(?:c|d)(?:)(?:a(?:)(?:b)(?:b(?:))(?:b(?:)(?:b)))/;
    var input0 = "cabbbb";
    var results = ["cabbbb"];
    shouldBe("regex540.exec(input0);", "results");

    var regex541 = /(?:c|d)(?:)(?:aaaaaaaa(?:)(?:bbbbbbbb)(?:bbbbbbbb(?:))(?:bbbbbbbb(?:)(?:bbbbbbbb)))/;
    var input0 = "caaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    var results = ["caaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"];
    shouldBe("regex541.exec(input0);", "results");

    var regex542 = /(ab)\d\1/i;
    var input0 = "Ab4ab";
    var results = ["Ab4ab", "Ab"];
    shouldBe("regex542.exec(input0);", "results");
    var input1 = "ab4Ab";
    var results = ["ab4Ab", "ab"];
    shouldBe("regex542.exec(input1);", "results");

    var regex543 = /foo\w*\d{4}baz/;
    var input0 = "foobar1234baz";
    var results = ["foobar1234baz"];
    shouldBe("regex543.exec(input0);", "results");

    var regex544 = /x(~~)*(?:(?:F)?)?/;
    var input0 = "x~~";
    var results = ["x~~", "~~"];
    shouldBe("regex544.exec(input0);", "results");

    var regex545 = /^a{3}c/;
    var input0 = "aaac";
    var results = ["aaac"];
    shouldBe("regex545.exec(input0);", "results");

    var regex550 = /^(?:a?b?)*$/;
    var input0 = "";
    var results = [""];
    shouldBe("regex550.exec(input0);", "results");
    var input1 = "a";
    var results = ["a"];
    shouldBe("regex550.exec(input1);", "results");
    var input2 = "ab";
    var results = ["ab"];
    shouldBe("regex550.exec(input2);", "results");
    var input3 = "aaa";
    var results = ["aaa"];
    shouldBe("regex550.exec(input3);", "results");
    // Failers
    var input4 = "dbcb";
    var results = null;
    shouldBe("regex550.exec(input4);", "results");
    var input5 = "a--";
    var results = null;
    shouldBe("regex550.exec(input5);", "results");
    var input6 = "aa--";
    var results = null;
    shouldBe("regex550.exec(input6);", "results");

    var regex551 = /(^a([\w\W]))(^b$)/m;
    var input0 = "a\nb\nc\n";
    var results = ["a\x0ab", "a\x0a", "\x0a", "b"];
    shouldBe("regex551.exec(input0);", "results");

    var regex552 = /(^b$)/m;
    var input0 = "a\nb\nc\n";
    var results = ["b", "b"];
    shouldBe("regex552.exec(input0);", "results");

    var regex553 = /^b/m;
    var input0 = "a\nb\n";
    var results = ["b"];
    shouldBe("regex553.exec(input0);", "results");

    var regex554 = /^(b)/m;
    var input0 = "a\nb\n";
    var results = ["b", "b"];
    shouldBe("regex554.exec(input0);", "results");

    var regex555 = /(^b)/m;
    var input0 = "a\nb\n";
    var results = ["b", "b"];
    shouldBe("regex555.exec(input0);", "results");

    var regex556 = /\n(^b)/m;
    var input0 = "a\nb\n";
    var results = ["\x0ab", "b"];
    shouldBe("regex556.exec(input0);", "results");

    var regex557 = /([\w\W])c(?!.)/m;
    var input0 = "a\nb\nc\n";
    var results = ["\x0ac", "\x0a"];
    shouldBe("regex557.exec(input0);", "results");
    var input1 = "a\nb\nc\n";
    var results = ["\x0ac", "\x0a"];
    shouldBe("regex557.exec(input1);", "results");

    var regex558 = /(b[\w\W])c(?!.)/m;
    var input0 = "a\nb\nc\n";
    var results = ["b\x0ac", "b\x0a"];
    shouldBe("regex558.exec(input0);", "results");
    var input1 = "a\nb\nc\n";
    var results = ["b\x0ac", "b\x0a"];
    shouldBe("regex558.exec(input1);", "results");

    var regex559 = /()^b/;
    // Failers
    var input0 = "a\nb\nc\n";
    var results = null;
    shouldBe("regex559.exec(input0);", "results");
    var input1 = "a\nb\nc\n";
    var results = null;
    shouldBe("regex559.exec(input1);", "results");

    var regex560 = /(^b)/m;
    var input0 = "a\nb\nc\n";
    var results = ["b", "b"];
    shouldBe("regex560.exec(input0);", "results");

    var regex561 = /(?:b|a)/;
    var input0 = "a";
    var results = ["a"];
    shouldBe("regex561.exec(input0);", "results");

    var regex562 = /(x)?(?:a|b)/;
    var input0 = "xa";
    var results = ["xa", "x"];
    shouldBe("regex562.exec(input0);", "results");
    var input1 = "a";
    var results = ["a", undefined];
    shouldBe("regex562.exec(input1);", "results");

    var regex563 = /(x)?(?:b|a)/;
    var input0 = "a";
    var results = ["a", undefined];
    shouldBe("regex563.exec(input0);", "results");

    var regex564 = /()?(?:b|a)/;
    var input0 = "a";
    var results = ["a", undefined];
    shouldBe("regex564.exec(input0);", "results");

    var regex565 = /()?(?:a|b)/;
    var input0 = "a";
    var results = ["a", undefined];
    shouldBe("regex565.exec(input0);", "results");

    var regex566 = /^(\()?blah(?:(\))?)$/;
    var input0 = "(blah)";
    var results = ["(blah)", "(", ")"];
    shouldBe("regex566.exec(input0);", "results");
    var input1 = "blah";
    var results = ["blah", undefined, undefined];
    shouldBe("regex566.exec(input1);", "results");
    var input2 = "blah)";
    var results = ["blah)", undefined, ")"];
    shouldBe("regex566.exec(input2);", "results");
    var input3 = "(blah";
    var results = ["(blah", "(", undefined];
    shouldBe("regex566.exec(input3);", "results");
    // Failers
    var input4 = "a";
    var results = null;
    shouldBe("regex566.exec(input4);", "results");

    var regex567 = /^(\(+)?blah(?:(\))?)$/;
    var input0 = "(blah)";
    var results = ["(blah)", "(", ")"];
    shouldBe("regex567.exec(input0);", "results");
    var input1 = "blah";
    var results = ["blah", undefined, undefined];
    shouldBe("regex567.exec(input1);", "results");
    var input2 = "blah)";
    var results = ["blah)", undefined, ")"];
    shouldBe("regex567.exec(input2);", "results");
    var input3 = "(blah";
    var results = ["(blah", "(", undefined];
    shouldBe("regex567.exec(input3);", "results");

    var regex568 = /((?!a)b|a)/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex568.exec(input0);", "results");

    var regex569 = /((?=a)b|a)/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex569.exec(input0);", "results");
    var input1 = "a";
    var results = ["a", "a"];
    shouldBe("regex569.exec(input1);", "results");

    var regex570 = /((?=a)a|b)/;
    var input0 = "a";
    var results = ["a", "a"];
    shouldBe("regex570.exec(input0);", "results");

    var regex571 = /(?=(a+?))(\1ab)/;
    var input0 = "aaab";
    var results = ["aab", "a", "aab"];
    shouldBe("regex571.exec(input0);", "results");

    var regex572 = /(\w+:)+/;
    var input0 = "one:";
    var results = ["one:", "one:"];
    shouldBe("regex572.exec(input0);", "results");

    var regex574 = /(?=(a+?))(\1ab)/;
    var input0 = "aaab";
    var results = ["aab", "a", "aab"];
    shouldBe("regex574.exec(input0);", "results");

    var regex575 = /^(?=(a+?))\1ab/;
    // Failers
    var input0 = "aaab";
    var results = null;
    shouldBe("regex575.exec(input0);", "results");
    var input1 = "aaab";
    var results = null;
    shouldBe("regex575.exec(input1);", "results");

    var regex576 = /([\w:]+::)?(\w+)$/;
    var input0 = "abcd";
    var results = ["abcd", undefined, "abcd"];
    shouldBe("regex576.exec(input0);", "results");
    var input1 = "xy:z:::abcd";
    var results = ["xy:z:::abcd", "xy:z:::", "abcd"];
    shouldBe("regex576.exec(input1);", "results");

    var regex577 = /^[^bcd]*(c+)/;
    var input0 = "aexycd";
    var results = ["aexyc", "c"];
    shouldBe("regex577.exec(input0);", "results");

    var regex578 = /(a*)b+/;
    var input0 = "caab";
    var results = ["aab", "aa"];
    shouldBe("regex578.exec(input0);", "results");

    var regex579 = /([\w:]+::)?(\w+)$/;
    var input0 = "abcd";
    var results = ["abcd", undefined, "abcd"];
    shouldBe("regex579.exec(input0);", "results");
    var input1 = "xy:z:::abcd";
    var results = ["xy:z:::abcd", "xy:z:::", "abcd"];
    shouldBe("regex579.exec(input1);", "results");
    // Failers
    var input2 = "abcd:";
    var results = null;
    shouldBe("regex579.exec(input2);", "results");
    var input3 = "abcd:";
    var results = null;
    shouldBe("regex579.exec(input3);", "results");

    var regex580 = /^[^bcd]*(c+)/;
    var input0 = "aexycd";
    var results = ["aexyc", "c"];
    shouldBe("regex580.exec(input0);", "results");

    var regex582 = /([[:]+)/;
    var input0 = "a:[b]:";
    var results = [":[", ":["];
    shouldBe("regex582.exec(input0);", "results");

    var regex583 = /([[=]+)/;
    var input0 = "a=[b]=";
    var results = ["=[", "=["];
    shouldBe("regex583.exec(input0);", "results");

    var regex584 = /([[.]+)/;
    var input0 = "a.[b].";
    var results = [".[", ".["];
    shouldBe("regex584.exec(input0);", "results");

    var regex588 = /a$/;
    var input0 = "aaab";
    var results = null;
    shouldBe("regex588.exec(input0);", "results");
    var input1 = "a\nb\n";
    var results = null;
    shouldBe("regex588.exec(input1);", "results");

    var regex589 = /b$/;
    var input0 = "a\nb";
    var results = ["b"];
    shouldBe("regex589.exec(input0);", "results");
    // Failers
    var input1 = "a\nb\n";
    var results = null;
    shouldBe("regex589.exec(input1);", "results");

    var regex590 = /b$/m;
    var input0 = "a\nb";
    var results = ["b"];
    shouldBe("regex590.exec(input0);", "results");
    var input1 = "a\nb\n";
    var results = ["b"];
    shouldBe("regex590.exec(input1);", "results");

    var regex600 = /<a[\s]+href[\s]*=[\s]*([\"\'])?(?:(.*?)\1|([^\s]+))/i;
    var input0 = "<a href=abcd xyz";
    var results = ["<a href=", undefined, "", undefined];
    shouldBe("regex600.exec(input0);", "results");
    var input1 = '<a href="abcd xyz pqr" cats';
    var results = ['<a href="abcd xyz pqr"', '"', "abcd xyz pqr", undefined];
    shouldBe("regex600.exec(input1);", "results");
    var input2 = "<a href=\'abcd xyz pqr\' cats";
    var results = ["<a href=\'abcd xyz pqr\'", "\'", "abcd xyz pqr", undefined];
    shouldBe("regex600.exec(input2);", "results");

    var regex601 = /<a\s+href\s*=\s*(["'])?(?:(.*?)\1|(\S+))/i;
    var input0 = "<a href=abcd xyz";
    var results = ["<a href=", undefined, "", undefined];
    shouldBe("regex601.exec(input0);", "results");
    var input1 = '<a href="abcd xyz pqr" cats';
    var results = ['<a href="abcd xyz pqr"', '"', "abcd xyz pqr", undefined];
    shouldBe("regex601.exec(input1);", "results");
    var input2 = "<a href       =       \'abcd xyz pqr\' cats";
    var results = ["<a href       =       \'abcd xyz pqr\'", "\'", "abcd xyz pqr", undefined];
    shouldBe("regex601.exec(input2);", "results");

    var regex603 = /((Z)+|A)*/;
    var input0 = "ZABCDEFG";
    var results = ["ZA", "A", undefined];
    shouldBe("regex603.exec(input0);", "results");

    var regex604 = /(Z()|A)*/;
    var input0 = "ZABCDEFG";
    var results = ["ZA", "A", undefined];
    shouldBe("regex604.exec(input0);", "results");

    var regex605 = /(Z(())|A)*/;
    var input0 = "ZABCDEFG";
    var results = ["ZA", "A", undefined, undefined];
    shouldBe("regex605.exec(input0);", "results");

    var regex608 = /^[a-\d]/;
    var input0 = "abcde";
    var results = ["a"];
    shouldBe("regex608.exec(input0);", "results");
    var input1 = "-things";
    var results = ["-"];
    shouldBe("regex608.exec(input1);", "results");
    var input2 = "0digit";
    var results = ["0"];
    shouldBe("regex608.exec(input2);", "results");
    // Failers
    var input3 = "bcdef";
    var results = null;
    shouldBe("regex608.exec(input3);", "results");

    var regex609 = /^[\d-a]/;
    var input0 = "abcde";
    var results = ["a"];
    shouldBe("regex609.exec(input0);", "results");
    var input1 = "-things";
    var results = ["-"];
    shouldBe("regex609.exec(input1);", "results");
    var input2 = "0digit";
    var results = ["0"];
    shouldBe("regex609.exec(input2);", "results");
    // Failers
    var input3 = "bcdef";
    var results = null;
    shouldBe("regex609.exec(input3);", "results");

    var regex610 = /[\s]+/;
    var input0 = "> \x09\x0a\x0c\x0d\x0b<";
    var results = [" \x09\x0a\x0c\x0d\x0b"];
    shouldBe("regex610.exec(input0);", "results");

    var regex611 = /[ ]+/;
    var input0 = "> \x09\x0a\x0c\x0d\x0b<";
    var results = [" "];
    shouldBe("regex611.exec(input0);", "results");

    var regex612 = /[\s]+/;
    var input0 = "> \x09\x0a\x0b\x0c\x0d\x20\xA0\u2028\u2029\uFEFF<";
    var results = [" \x09\x0a\x0b\x0c\x0d\x20\xA0\u2028\u2029\uFEFF"];
    shouldBe("regex612.exec(input0);", "results");

    var regex613 = /\s+/;
    var input0 = "> \x09\x0a\x0b\x0c\x0d\x20\xA0\u2028\u2029\uFEFF<";
    var results = [" \x09\x0a\x0b\x0c\x0d\x20\xA0\u2028\u2029\uFEFF"];
    shouldBe("regex613.exec(input0);", "results");

    var regex614 = /ab/;
    var input0 = "ab";
    var results = ["ab"];
    shouldBe("regex614.exec(input0);", "results");

    var regex616 = /(?!^)x/m;
    var input0 = "a\nxb\n";
    var results = null;
    shouldBe("regex616.exec(input0);", "results");

    var regex621 = /^abc/;
    var input0 = "abc";
    var results = ["abc"];
    shouldBe("regex621.exec(input0);", "results");
    // Failers
    var input1 = "xyzabc";
    var results = null;
    shouldBe("regex621.exec(input1);", "results");

    var regex622 = /a(?:bc)d/;
    var input0 = "XabcdY";
    var results = ["abcd"];
    shouldBe("regex622.exec(input0);", "results");
    // Failers
    var input1 = "Xa b c d Y";
    var results = null;
    shouldBe("regex622.exec(input1);", "results");

    var regex623 = /(xyz|abc)/;
    var input0 = "XabcY";
    var results = ["abc", "abc"];
    shouldBe("regex623.exec(input0);", "results");
    var input1 = "AxyzB";
    var results = ["xyz", "xyz"];
    shouldBe("regex623.exec(input1);", "results");

    var regex624 = /ABC/i;
    var input0 = "XabCY";
    var results = ["abC"];
    shouldBe("regex624.exec(input0);", "results");

    var regex625 = /([aA][bB]C|D)E/;
    var input0 = "abCE";
    var results = ["abCE", "abC"];
    shouldBe("regex625.exec(input0);", "results");
    var input1 = "DE";
    var results = ["DE", "D"];
    shouldBe("regex625.exec(input1);", "results");
    // Failers
    var input2 = "abcE";
    var results = null;
    shouldBe("regex625.exec(input2);", "results");
    var input3 = "abCe";
    var results = null;
    shouldBe("regex625.exec(input3);", "results");
    var input4 = "dE";
    var results = null;
    shouldBe("regex625.exec(input4);", "results");
    var input5 = "De";
    var results = null;
    shouldBe("regex625.exec(input5);", "results");

    var regex626 = /(.*)\d+\1/;
    var input0 = "abc123abc";
    var results = ["abc123abc", "abc"];
    shouldBe("regex626.exec(input0);", "results");
    var input1 = "abc123bc";
    var results = ["bc123bc", "bc"];
    shouldBe("regex626.exec(input1);", "results");

    var regex627 = /(.*)\d+\1/;
    var input0 = "abc123abc";
    var results = ["abc123abc", "abc"];
    shouldBe("regex627.exec(input0);", "results");
    var input1 = "abc123bc";
    var results = ["bc123bc", "bc"];
    shouldBe("regex627.exec(input1);", "results");

    var regex628 = /((.*))\d+\1/;
    var input0 = "abc123abc";
    var results = ["abc123abc", "abc", "abc"];
    shouldBe("regex628.exec(input0);", "results");
    var input1 = "abc123bc";
    var results = ["bc123bc", "bc", "bc"];
    shouldBe("regex628.exec(input1);", "results");

    var regex630 = /[\z\C]/;
    var input0 = "z";
    var results = ["z"];
    shouldBe("regex630.exec(input0);", "results");
    var input1 = "C";
    var results = ["C"];
    shouldBe("regex630.exec(input1);", "results");

    var regex631 = /\M/;
    var input0 = "M";
    var results = ["M"];
    shouldBe("regex631.exec(input0);", "results");

    var regex632 = /(a+)*b/;
    var input0 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex632.exec(input0);", "results");

    var regex633 = /reg(?:ul(?:[a\xc3\xa4]|ae)r|ex)/i;
    var input0 = "REGular";
    var results = ["REGular"];
    shouldBe("regex633.exec(input0);", "results");
    var input1 = "regulaer";
    var results = ["regulaer"];
    shouldBe("regex633.exec(input1);", "results");
    var input2 = "Regex";
    var results = ["Regex"];
    shouldBe("regex633.exec(input2);", "results");
    var input3 = "regul\xa4r";
    var results = ["regul\xa4r"];
    shouldBe("regex633.exec(input3);", "results");
    // Failers
    var input4 = "regul\xc3\xa4r";
    var results = null;
    shouldBe("regex633.exec(input4);", "results");

    var regex634 = /\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4[\xc3\xa0-\xc3\xbf\xc3\x80-\xc3\x9f]+/;
    var input0 = "\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3";
    var results = ["\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3"];
    shouldBe("regex634.exec(input0);", "results");
    var input1 = "\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\xbf";
    var results = ["\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\xbf"];
    shouldBe("regex634.exec(input1);", "results");
    var input2 = "\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\x80";
    var results = ["\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\x80"];
    shouldBe("regex634.exec(input2);", "results");
    var input3 = "\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\x9f";
    var results = ["\xc3\x85\xc3\xa6\xc3\xa5\xc3\xa4\xc3\x9f"];
    shouldBe("regex634.exec(input3);", "results");

    var regex636 = /ab cd defg/;
    var input0 = "ab cd defg";
    var results = ["ab cd defg"];
    shouldBe("regex636.exec(input0);", "results");

    var regex637 = /ab cddefg/;
    var input0 = "ab cddefg";
    var results = ["ab cddefg"];
    shouldBe("regex637.exec(input0);", "results");
    // Failers
    var input1 = "abcddefg";
    var results = null;
    shouldBe("regex637.exec(input1);", "results");

    var regex641 = /(?:(?:a|b)(X))+/;
    var input0 = "bXaX";
    var results = ["bXaX", "X"];
    shouldBe("regex641.exec(input0);", "results");

    var regex642 = /(?:(?:\1a|b)(X|Y))+/;
    var input0 = "bXXaYYaY";
    var results = ["bX", "X"];
    shouldBe("regex642.exec(input0);", "results");
    var input1 = "bXYaXXaX";
    var results = ["bX", "X"];
    shouldBe("regex642.exec(input1);", "results");

    var regex643 = /()()()()()()()()()(?:(\10a|b)(X|Y))+/;
    var input0 = "bXXaYYaY";
    var results = ["bX", "", "", "", "", "", "", "", "", "", "b", "X"];
    shouldBe("regex643.exec(input0);", "results");

    var regex644 = /[[,abc,]+]/;
    var input0 = "abc]";
    var results = ["abc]"];
    shouldBe("regex644.exec(input0);", "results");
    var input1 = "a,b]";
    var results = ["a,b]"];
    shouldBe("regex644.exec(input1);", "results");
    var input2 = "[a,b,c]";
    var results = ["[a,b,c]"];
    shouldBe("regex644.exec(input2);", "results");

    var regex645 = /a*b*\w/;
    var input0 = "aaabbbb";
    var results = ["aaabbbb"];
    shouldBe("regex645.exec(input0);", "results");
    var input1 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex645.exec(input1);", "results");
    var input2 = "a";
    var results = ["a"];
    shouldBe("regex645.exec(input2);", "results");

    var regex646 = /a*b?\w/;
    var input0 = "aaabbbb";
    var results = ["aaabb"];
    shouldBe("regex646.exec(input0);", "results");
    var input1 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex646.exec(input1);", "results");
    var input2 = "a";
    var results = ["a"];
    shouldBe("regex646.exec(input2);", "results");

    var regex647 = /a*b{0,4}\w/;
    var input0 = "aaabbbb";
    var results = ["aaabbbb"];
    shouldBe("regex647.exec(input0);", "results");
    var input1 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex647.exec(input1);", "results");
    var input2 = "a";
    var results = ["a"];
    shouldBe("regex647.exec(input2);", "results");

    var regex648 = /a*b{0,}\w/;
    var input0 = "aaabbbb";
    var results = ["aaabbbb"];
    shouldBe("regex648.exec(input0);", "results");
    var input1 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex648.exec(input1);", "results");
    var input2 = "a";
    var results = ["a"];
    shouldBe("regex648.exec(input2);", "results");

    var regex649 = /a*\d*\w/;
    var input0 = "0a";
    var results = ["0a"];
    shouldBe("regex649.exec(input0);", "results");
    var input1 = "a";
    var results = ["a"];
    shouldBe("regex649.exec(input1);", "results");

    var regex650 = /^\w+=.*(\\\n.*)+/;
    var input0 = "abc=xyz\\\npqr";
    var results = ["abc=xyz\\\npqr", "\\\npqr"];
    shouldBe("regex650.exec(input0);", "results");

    var regex651 = /(?=(\w+))\1:/;
    var input0 = "abcd:";
    var results = ["abcd:", "abcd"];
    shouldBe("regex651.exec(input0);", "results");

    var regex652 = /^(?=(\w+))\1:/;
    var input0 = "abcd:";
    var results = ["abcd:", "abcd"];
    shouldBe("regex652.exec(input0);", "results");

    var regex660 = /^(a()*)*/;
    var input0 = "aaaa";
    var results = ["aaaa", "a", undefined];
    shouldBe("regex660.exec(input0);", "results");

    var regex661 = /^(?:a(?:(?:))*)*/;
    var input0 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex661.exec(input0);", "results");

    var regex662 = /^(a()+)+/;
    var input0 = "aaaa";
    var results = ["aaaa", "a", ""];
    shouldBe("regex662.exec(input0);", "results");

    var regex663 = /^(?:a(?:(?:))+)+/;
    var input0 = "aaaa";
    var results = ["aaaa"];
    shouldBe("regex663.exec(input0);", "results");

    var regex664 = /(a){0,3}(?:b|(c|))*D/;
    var input0 = "abbD";
    var results = ["abbD", "a", undefined];
    shouldBe("regex664.exec(input0);", "results");
    var input1 = "ccccD";
    var results = ["ccccD", undefined, "c"];
    shouldBe("regex664.exec(input1);", "results");
    var input2 = "D";
    var results = ["D", undefined, undefined];
    shouldBe("regex664.exec(input2);", "results");

    var regex665 = /(a|)*\d/;
    var input0 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex665.exec(input0);", "results");
    var input1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4";
    var results = ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4", "a"];
    shouldBe("regex665.exec(input1);", "results");

    var regex667 = /(?:a|)*\d/;
    var input0 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var results = null;
    shouldBe("regex667.exec(input0);", "results");
    var input1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4";
    var results = ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4"];
    shouldBe("regex667.exec(input1);", "results");

    var regex669 = /^(?![^\n]*\n$)/;
    var input0 = "abc";
    var results = [""];
    shouldBe("regex669.exec(input0);", "results");
    var input1 = "abc\n";
    var results = null;
    shouldBe("regex669.exec(input1);", "results");

    var regex671 = /(.*(.)?)*/;
    var input0 = "abcd";
    var results = ["abcd", "abcd", undefined];
    shouldBe("regex671.exec(input0);", "results");

    var regex672 = /[[:abcd:xyz]]/;
    var input0 = "a]";
    var results = ["a]"];
    shouldBe("regex672.exec(input0);", "results");
    var input1 = ":]";
    var results = [":]"];
    shouldBe("regex672.exec(input1);", "results");

    var regex673 = /[abc[:x\]pqr]/;
    var input0 = "a";
    var results = ["a"];
    shouldBe("regex673.exec(input0);", "results");
    var input1 = "[";
    var results = ["["];
    shouldBe("regex673.exec(input1);", "results");
    var input2 = ":";
    var results = [":"];
    shouldBe("regex673.exec(input2);", "results");
    var input3 = "]";
    var results = ["]"];
    shouldBe("regex673.exec(input3);", "results");
    var input4 = "p";
    var results = ["p"];
    shouldBe("regex673.exec(input4);", "results");

    var regex674 = /(a(b)*)*/;
    var input0 = "aba";
    var results = ["aba", "a", undefined];
    shouldBe("regex674.exec(input0);", "results");

    var regex675 = /(a*)*/;
    var input0 = "ab";
    var results = ["a", "a"];
    shouldBe("regex675.exec(input0);", "results");

    var regex676 = /([ab]*)*/;
    var input0 = "abab";
    var results = ["abab", "abab"];
    shouldBe("regex676.exec(input0);", "results");

    // Global matches.

    var regexGlobal0 = RegExp("", "g");
    var input0 = "abc";
    var results = ["", "", "", ""];
    shouldBe("input0.match(regexGlobal0);", "results");

    var regexGlobal1 = /a*/g;
    var input0 = "abbab";
    var results = ["a", "", "", "a", "", ""];
    shouldBe("input0.match(regexGlobal1);", "results");

    var regexGlobal2 = /\babc./g;
    var input0 = "abc:abc;xyzabc.";
    var results = ["abc:", "abc;"];
    shouldBe("input0.match(regexGlobal2);", "results");

    var regexGlobal3 = /abc./g;
    var input0 = "abc:abc;xyzabc.";
    var results = ["abc:", "abc;", "abc."];
    shouldBe("input0.match(regexGlobal3);", "results");

    var regexGlobal4 = /$/g;
    var input0 = "abc\n";
    var results = [""];
    shouldBe("input0.match(regexGlobal4);", "results");

    var regexGlobal5 = /$/gm;
    var input0 = "abc\n";
    var results = ["", ""];
    shouldBe("input0.match(regexGlobal5);", "results");

    var regexGlobal6 = /^/g;
    var input0 = "a\nb\nc\n";
    var results = [""];
    shouldBe("input0.match(regexGlobal6);", "results");
    var input1 = "\\";
    var results = [""];
    shouldBe("input1.match(regexGlobal6);", "results");

    var regexGlobal7 = /^/gm;
    var input0 = "a\nb\nc\n";
    var results = ["", "", "", ""];
    shouldBe("input0.match(regexGlobal7);", "results");
    var input1 = "\\";
    var results = [""];
    shouldBe("input1.match(regexGlobal7);", "results");

    var regexGlobal8 = /^(?=C)/g;
    // Failers
    var input0 = "A\nC\nC\nE\n";
    var results = null;
    shouldBe("input0.match(regexGlobal8);", "results");

    var regexGlobal9 = /^(?=C)/gm;
    var input0 = "A\nC\nC\nE\n";
    var results = ["", ""];
    shouldBe("input0.match(regexGlobal9);", "results");

    // DISABLED:
    // These tests use (?<) or (?>) constructs. These are not currently valid in ECMAScript,
    // but these tests may be useful if similar constructs are introduced in the future.

    //var regex217 = /(?<!bar)foo/;
    //var input0 = "foo";
    //var results = ["foo"];
    //shouldBe('regex217.exec(input0);', 'results');
    //var input1 = "catfood";
    //var results = ["foo"];
    //shouldBe('regex217.exec(input1);', 'results');
    //var input2 = "arfootle";
    //var results = ["foo"];
    //shouldBe('regex217.exec(input2);', 'results');
    //var input3 = "rfoosh";
    //var results = ["foo"];
    //shouldBe('regex217.exec(input3);', 'results');
    //// Failers
    //var input4 = "barfoo";
    //var results = null;
    //shouldBe('regex217.exec(input4);', 'results');
    //var input5 = "towbarfoo";
    //var results = null;
    //shouldBe('regex217.exec(input5);', 'results');
    //
    //var regex218 = /\w{3}(?<!bar)foo/;
    //var input0 = "catfood";
    //var results = ["catfoo"];
    //shouldBe('regex218.exec(input0);', 'results');
    //// Failers
    //var input1 = "foo";
    //var results = null;
    //shouldBe('regex218.exec(input1);', 'results');
    //var input2 = "barfoo";
    //var results = null;
    //shouldBe('regex218.exec(input2);', 'results');
    //var input3 = "towbarfoo";
    //var results = null;
    //shouldBe('regex218.exec(input3);', 'results');
    //
    //var regex219 = /(?<=(foo)a)bar/;
    //var input0 = "fooabar";
    //var results = ["bar", "foo"];
    //shouldBe('regex219.exec(input0);', 'results');
    //// Failers
    //var input1 = "bar";
    //var results = null;
    //shouldBe('regex219.exec(input1);', 'results');
    //var input2 = "foobbar";
    //var results = null;
    //shouldBe('regex219.exec(input2);', 'results');
    //
    //var regex221 = /(?>.*\/)foo/;
    //var input0 = "/this/is/a/very/long/line/in/deed/with/very/many/slashes/in/it/you/see/";
    //var results = null;
    //shouldBe('regex221.exec(input0);', 'results');
    //
    //var regex222 = /(?>.*\/)foo/;
    //var input0 = "/this/is/a/very/long/line/in/deed/with/very/many/slashes/in/and/foo";
    //var results = ["/this/is/a/very/long/line/in/deed/with/very/many/slashes/in/and/foo"];
    //shouldBe('regex222.exec(input0);', 'results');
    //
    //var regex223 = /(?>(\.\d\d[1-9]?))\d+/;
    //var input0 = "1.230003938";
    //var results = [".230003938", ".23"];
    //shouldBe('regex223.exec(input0);', 'results');
    //var input1 = "1.875000282";
    //var results = [".875000282", ".875"];
    //shouldBe('regex223.exec(input1);', 'results');
    //// Failers
    //var input2 = "1.235";
    //var results = null;
    //shouldBe('regex223.exec(input2);', 'results');
    //
    //var regex224 = /^((?>\w+)|(?>\s+))*$/;
    //var input0 = "now is the time for all good men to come to the aid of the party";
    //var results = ["now is the time for all good men to come to the aid of the party", "party"];
    //shouldBe('regex224.exec(input0);', 'results');
    //// Failers
    //var input1 = "this is not a line with only words and spaces!";
    //var results = null;
    //shouldBe('regex224.exec(input1);', 'results');
    //
    //var regex226 = /((?>\d+))(\w)/;
    //var input0 = "12345a";
    //var results = ["12345a", "12345", "a"];
    //shouldBe('regex226.exec(input0);', 'results');
    //// Failers
    //var input1 = "12345+";
    //var results = null;
    //shouldBe('regex226.exec(input1);', 'results');
    //
    //var regex227 = /(?>a+)b/;
    //var input0 = "aaab";
    //var results = ["aaab"];
    //shouldBe('regex227.exec(input0);', 'results');
    //
    //var regex228 = /((?>a+)b)/;
    //var input0 = "aaab";
    //var results = ["aaab", "aaab"];
    //shouldBe('regex228.exec(input0);', 'results');
    //
    //var regex229 = /(?>(a+))b/;
    //var input0 = "aaab";
    //var results = ["aaab", "aaa"];
    //shouldBe('regex229.exec(input0);', 'results');
    //
    //var regex230 = /(?>b)+/;
    //var input0 = "aaabbbccc";
    //var results = ["bbb"];
    //shouldBe('regex230.exec(input0);', 'results');
    //
    //var regex231 = /(?>a+|b+|c+)*c/;
    //var input0 = "aaabbbbccccd";
    //var results = ["aaabbbbc"];
    //shouldBe('regex231.exec(input0);', 'results');
    //
    //var regex232 = /((?>[^()]+)|\([^()]*\))+/;
    //var input0 = "((abc(ade)ufh()()x";
    //var results = ["abc(ade)ufh()()x", "x"];
    //shouldBe('regex232.exec(input0);', 'results');
    //
    //var regex233 = /\(((?>[^()]+)|\([^()]+\))+\)/ ;
    //var input0 = "(abc)";
    //var results = ["(abc)", "abc"];
    //shouldBe('regex233.exec(input0);', 'results');
    //var input1 = "(abc(def)xyz)";
    //var results = ["(abc(def)xyz)", "xyz"];
    //shouldBe('regex233.exec(input1);', 'results');
    //// Failers
    //var input2 = "((()aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    //var results = null;
    //shouldBe('regex233.exec(input2);', 'results');
    //
    //var regex245 = /(?<=ab)(\w\w)c/;
    //var input0 = "abxxc";
    //var results = ["xxc", "xx"];
    //shouldBe('regex245.exec(input0);', 'results');
    //var input1 = "aBxxc";
    //var results = ["xxc", "xx"];
    //shouldBe('regex245.exec(input1);', 'results');
    //// Failers
    //var input2 = "Abxxc";
    //var results = null;
    //shouldBe('regex245.exec(input2);', 'results');
    //var input3 = "ABxxc";
    //var results = null;
    //shouldBe('regex245.exec(input3);', 'results');
    //var input4 = "abxxC";
    //var results = null;
    //shouldBe('regex245.exec(input4);', 'results');
    //
    //var regex250 = /(?(?<=foo)bar|cat)/;
    //var input0 = "foobar";
    //var results = ["bar"];
    //shouldBe('regex250.exec(input0);', 'results');
    //var input1 = "cat";
    //var results = ["cat"];
    //shouldBe('regex250.exec(input1);', 'results');
    //var input2 = "fcat";
    //var results = ["cat"];
    //shouldBe('regex250.exec(input2);', 'results');
    //var input3 = "focat";
    //var results = ["cat"];
    //shouldBe('regex250.exec(input3);', 'results');
    //// Failers
    //var input4 = "foocat";
    //var results = null;
    //shouldBe('regex250.exec(input4);', 'results');
    //
    //var regex251 = /(?(?<!foo)cat|bar)/;
    //var input0 = "foobar";
    //var results = ["bar"];
    //shouldBe('regex251.exec(input0);', 'results');
    //var input1 = "cat";
    //var results = ["cat"];
    //shouldBe('regex251.exec(input1);', 'results');
    //var input2 = "fcat";
    //var results = ["cat"];
    //shouldBe('regex251.exec(input2);', 'results');
    //var input3 = "focat";
    //var results = ["cat"];
    //shouldBe('regex251.exec(input3);', 'results');
    //// Failers
    //var input4 = "foocat";
    //var results = null;
    //shouldBe('regex251.exec(input4);', 'results');
    //
    //var regex256 = /(?>a*)*/;
    //var input0 = "a";
    //var results = ["a"];
    //shouldBe('regex256.exec(input0);', 'results');
    //var input1 = "aa";
    //var results = ["aa"];
    //shouldBe('regex256.exec(input1);', 'results');
    //var input2 = "aaaa";
    //var results = ["aaaa"];
    //shouldBe('regex256.exec(input2);', 'results');
    //
    //var regex266 = /(?>a*)*/;
    //var input0 = "a";
    //var results = ["a"];
    //shouldBe('regex266.exec(input0);', 'results');
    //var input1 = "aaabcde";
    //var results = ["aaa"];
    //shouldBe('regex266.exec(input1);', 'results');
    //
    //var regex267 = /((?>a*))*/;
    //var input0 = "aaaaa";
    //var results = ["aaaaa", ""];
    //shouldBe('regex267.exec(input0);', 'results');
    //var input1 = "aabbaa";
    //var results = ["aa", ""];
    //shouldBe('regex267.exec(input1);', 'results');
    //
    //var regex268 = /((?>a*?))*/;
    //var input0 = "aaaaa";
    //var results = ["", ""];
    //shouldBe('regex268.exec(input0);', 'results');
    //var input1 = "aabbaa";
    //var results = ["", ""];
    //shouldBe('regex268.exec(input1);', 'results');
    //
    //var regex270 = /(?<=(foo))bar\1/;
    //var input0 = "foobarfoo";
    //var results = ["barfoo", "foo"];
    //shouldBe('regex270.exec(input0);', 'results');
    //var input1 = "foobarfootling";
    //var results = ["barfoo", "foo"];
    //shouldBe('regex270.exec(input1);', 'results');
    //// Failers
    //var input2 = "foobar";
    //var results = null;
    //shouldBe('regex270.exec(input2);', 'results');
    //var input3 = "barfoo";
    //var results = null;
    //shouldBe('regex270.exec(input3);', 'results');
    //
    //var regex275 = /(?<=foo\n)^bar/m;
    //var input0 = "foo\nbar";
    //var results = ["bar"];
    //shouldBe('regex275.exec(input0);', 'results');
    //// Failers
    //var input1 = "bar";
    //var results = null;
    //shouldBe('regex275.exec(input1);', 'results');
    //var input2 = "baz\nbar";
    //var results = null;
    //shouldBe('regex275.exec(input2);', 'results');
    //
    //var regex276 = /(?<=(?<!foo)bar)baz/;
    //var input0 = "barbaz";
    //var results = ["baz"];
    //shouldBe('regex276.exec(input0);', 'results');
    //var input1 = "barbarbaz";
    //var results = ["baz"];
    //shouldBe('regex276.exec(input1);', 'results');
    //var input2 = "koobarbaz";
    //var results = ["baz"];
    //shouldBe('regex276.exec(input2);', 'results');
    //// Failers
    //var input3 = "baz";
    //var results = null;
    //shouldBe('regex276.exec(input3);', 'results');
    //var input4 = "foobarbaz";
    //var results = null;
    //shouldBe('regex276.exec(input4);', 'results');
    //
    //var regex501 = /(?<=a)b/;
    //var input0 = "ab";
    //var results = ["b"];
    //shouldBe('regex501.exec(input0);', 'results');
    //// Failers
    //var input1 = "cb";
    //var results = null;
    //shouldBe('regex501.exec(input1);', 'results');
    //var input2 = "b";
    //var results = null;
    //shouldBe('regex501.exec(input2);', 'results');
    //
    //var regex502 = /(?<!c)b/;
    //var input0 = "ab";
    //var results = ["b"];
    //shouldBe('regex502.exec(input0);', 'results');
    //var input1 = "b";
    //var results = ["b"];
    //shouldBe('regex502.exec(input1);', 'results');
    //var input2 = "b";
    //var results = ["b"];
    //shouldBe('regex502.exec(input2);', 'results');
    //
    //var regex546 = /(?<![cd])b/;
    //// Failers
    //var input0 = "B\nB";
    //var results = null;
    //shouldBe('regex546.exec(input0);', 'results');
    //var input1 = "dbcb";
    //var results = null;
    //shouldBe('regex546.exec(input1);', 'results');
    //
    //var regex547 = /(?<![cd])[ab]/;
    //var input0 = "dbaacb";
    //var results = ["a"];
    //shouldBe('regex547.exec(input0);', 'results');
    //
    //var regex548 = /(?<!(c|d))[ab]/;
    //var input0 = "dbaacb";
    //var results = ["a"];
    //shouldBe('regex548.exec(input0);', 'results');
    //
    //var regex549 = /(?<!cd)[ab]/;
    //var input0 = "cdaccb";
    //var results = ["b"];
    //shouldBe('regex549.exec(input0);', 'results');
    //
    //var regex573 = /$(?<=^(a))/;
    //var input0 = "a";
    //var results = ["", "a"];
    //shouldBe('regex573.exec(input0);', 'results');
    //
    //var regex581 = /(?>a+)b/;
    //var input0 = "aaab";
    //var results = ["aaab"];
    //shouldBe('regex581.exec(input0);', 'results');
    //
    //var regex585 = /((?>a+)b)/;
    //var input0 = "aaab";
    //var results = ["aaab", "aaab"];
    //shouldBe('regex585.exec(input0);', 'results');
    //
    //var regex586 = /(?>(a+))b/;
    //var input0 = "aaab";
    //var results = ["aaab", "aaa"];
    //shouldBe('regex586.exec(input0);', 'results');
    //
    //var regex587 = /((?>[^()]+)|\([^()]*\))+/;
    //var input0 = "((abc(ade)ufh()()x";
    //var results = ["abc(ade)ufh()()x", "x"];
    //shouldBe('regex587.exec(input0);', 'results');
    //
    //var regex592 = /^(?>(?(1)\.|())[^\W_](?>[a-z0-9-]*[^\W_])?)+$/;
    //var input0 = "a";
    //var results = ["a", ""];
    //shouldBe('regex592.exec(input0);', 'results');
    //var input1 = "abc";
    //var results = ["abc", ""];
    //shouldBe('regex592.exec(input1);', 'results');
    //var input2 = "a-b";
    //var results = ["a-b", ""];
    //shouldBe('regex592.exec(input2);', 'results');
    //var input3 = "0-9";
    //var results = ["0-9", ""];
    //shouldBe('regex592.exec(input3);', 'results');
    //var input4 = "a.b";
    //var results = ["a.b", ""];
    //shouldBe('regex592.exec(input4);', 'results');
    //var input5 = "5.6.7";
    //var results = ["5.6.7", ""];
    //shouldBe('regex592.exec(input5);', 'results');
    //var input6 = "the.quick.brown.fox";
    //var results = ["the.quick.brown.fox", ""];
    //shouldBe('regex592.exec(input6);', 'results');
    //var input7 = "a100.b200.300c";
    //var results = ["a100.b200.300c", ""];
    //shouldBe('regex592.exec(input7);', 'results');
    //var input8 = "12-ab.1245";
    //var results = ["12-ab.1245", ""];
    //shouldBe('regex592.exec(input8);', 'results');
    //// Failers
    //var input9 = "\";
    //var results = null;
    //shouldBe('regex592.exec(input9);', 'results');
    //var input10 = ".a";
    //var results = null;
    //shouldBe('regex592.exec(input10);', 'results');
    //var input11 = "-a";
    //var results = null;
    //shouldBe('regex592.exec(input11);', 'results');
    //var input12 = "a-";
    //var results = null;
    //shouldBe('regex592.exec(input12);', 'results');
    //var input13 = "a.";
    //var results = null;
    //shouldBe('regex592.exec(input13);', 'results');
    //var input14 = "a_b";
    //var results = null;
    //shouldBe('regex592.exec(input14);', 'results');
    //var input15 = "a.-";
    //var results = null;
    //shouldBe('regex592.exec(input15);', 'results');
    //var input16 = "a..";
    //var results = null;
    //shouldBe('regex592.exec(input16);', 'results');
    //var input17 = "ab..bc";
    //var results = null;
    //shouldBe('regex592.exec(input17);', 'results');
    //var input18 = "the.quick.brown.fox-";
    //var results = null;
    //shouldBe('regex592.exec(input18);', 'results');
    //var input19 = "the.quick.brown.fox.";
    //var results = null;
    //shouldBe('regex592.exec(input19);', 'results');
    //var input20 = "the.quick.brown.fox_";
    //var results = null;
    //shouldBe('regex592.exec(input20);', 'results');
    //var input21 = "the.quick.brown.fox+";
    //var results = null;
    //shouldBe('regex592.exec(input21);', 'results');
    //
    //var regex593 = /(?>.*)(?<=(abcd|wxyz))/;
    //var input0 = "alphabetabcd";
    //var results = ["alphabetabcd", "abcd"];
    //shouldBe('regex593.exec(input0);', 'results');
    //var input1 = "endingwxyz";
    //var results = ["endingwxyz", "wxyz"];
    //shouldBe('regex593.exec(input1);', 'results');
    //// Failers
    //var input2 = "a rather long string that doesn't end with one of them";
    //var results = null;
    //shouldBe('regex593.exec(input2);', 'results');
    //
    //var regex594 = /word (?>(?:(?!otherword)[a-zA-Z0-9]+ ){0,30})otherword/;
    //var input0 = "word cat dog elephant mussel cow horse canary baboon snake shark otherword";
    //var results = ["word cat dog elephant mussel cow horse canary baboon snake shark otherword"];
    //shouldBe('regex594.exec(input0);', 'results');
    //var input1 = "word cat dog elephant mussel cow horse canary baboon snake shark";
    //var results = null;
    //shouldBe('regex594.exec(input1);', 'results');
    //
    //var regex595 = /word (?>[a-zA-Z0-9]+ ){0,30}otherword/;
    //var input0 = "word cat dog elephant mussel cow horse canary baboon snake shark the quick brown fox and the lazy dog and several other words getting close to thirty by now I hope";
    //var results = null;
    //shouldBe('regex595.exec(input0);', 'results');
    //
    //var regex596 = /(?<=\d{3}(?!999))foo/;
    //var input0 = "999foo";
    //var results = ["foo"];
    //shouldBe('regex596.exec(input0);', 'results');
    //var input1 = "123999foo";
    //var results = ["foo"];
    //shouldBe('regex596.exec(input1);', 'results');
    //// Failers
    //var input2 = "123abcfoo";
    //var results = null;
    //shouldBe('regex596.exec(input2);', 'results');
    //
    //var regex597 = /(?<=(?!...999)\d{3})foo/;
    //var input0 = "999foo";
    //var results = ["foo"];
    //shouldBe('regex597.exec(input0);', 'results');
    //var input1 = "123999foo";
    //var results = ["foo"];
    //shouldBe('regex597.exec(input1);', 'results');
    //// Failers
    //var input2 = "123abcfoo";
    //var results = null;
    //shouldBe('regex597.exec(input2);', 'results');
    //
    //var regex598 = /(?<=\d{3}(?!999)...)foo/;
    //var input0 = "123abcfoo";
    //var results = ["foo"];
    //shouldBe('regex598.exec(input0);', 'results');
    //var input1 = "123456foo";
    //var results = ["foo"];
    //shouldBe('regex598.exec(input1);', 'results');
    //// Failers
    //var input2 = "123999foo";
    //var results = null;
    //shouldBe('regex598.exec(input2);', 'results');
    //
    //var regex599 = /(?<=\d{3}...)(?<!999)foo/;
    //var input0 = "123abcfoo";
    //var results = ["foo"];
    //shouldBe('regex599.exec(input0);', 'results');
    //var input1 = "123456foo";
    //var results = ["foo"];
    //shouldBe('regex599.exec(input1);', 'results');
    //// Failers
    //var input2 = "123999foo";
    //var results = null;
    //shouldBe('regex599.exec(input2);', 'results');
    //
    //var regex602 = /<a\s+href(?>\s*)=(?>\s*)(["'])?(?(1) (.*?)\1 | (\S+))/i;
    //var input0 = "<a href=abcd xyz";
    //var results = ["<a href=abcd", undefined, undefined, "abcd"];
    //shouldBe('regex602.exec(input0);', 'results');
    //var input1 = "<a href=\"abcd xyz pqr\" cats";
    //var results = ["<a href=\"abcd xyz pqr\"", "\"", "abcd xyz pqr"];
    //shouldBe('regex602.exec(input1);', 'results');
    //var input2 = "<a href       =       \'abcd xyz pqr\' cats";
    //var results = ["<a href       =       \'abcd xyz pqr\'", "\'", "abcd xyz pqr"];
    //shouldBe('regex602.exec(input2);', 'results');
    //
    //var regex606 = /((?>Z)+|A)*/;
    //var input0 = "ZABCDEFG";
    //var results = ["ZA", "A"];
    //shouldBe('regex606.exec(input0);', 'results');
    //
    //var regex607 = /((?>)+|A)*/;
    //var input0 = "ZABCDEFG";
    //var results = ["", ""];
    //shouldBe('regex607.exec(input0);', 'results');
    //
    //var regex635 = /(?<=Z)X./;
    //var input0 = "\x84XAZXB";
    //var results = ["XB"];
    //shouldBe('regex635.exec(input0);', 'results');
    //
    //var regex638 = /(?<![^f]oo)(bar)/;
    //var input0 = "foobarX";
    //var results = ["bar", "bar"];
    //shouldBe('regex638.exec(input0);', 'results');
    //// Failers
    //var input1 = "boobarX";
    //var results = null;
    //shouldBe('regex638.exec(input1);', 'results');
    //
    //var regex639 = /(?<![^f])X/;
    //var input0 = "offX";
    //var results = ["X"];
    //shouldBe('regex639.exec(input0);', 'results');
    //// Failers
    //var input1 = "onyX";
    //var results = null;
    //shouldBe('regex639.exec(input1);', 'results');
    //
    //var regex640 = /(?<=[^f])X/;
    //var input0 = "onyX";
    //var results = ["X"];
    //shouldBe('regex640.exec(input0);', 'results');
    //// Failers
    //var input1 = "offX";
    //var results = null;
    //shouldBe('regex640.exec(input1);', 'results');
    //
    //var regex666 = /(?>a|)*\d/;
    //var input0 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    //var results = null;
    //shouldBe('regex666.exec(input0);', 'results');
    //var input1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4";
    //var results = ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4"];
    //shouldBe('regex666.exec(input1);', 'results');
    //
    //var regex668 = /^(?>.*)(?<!\n)/;
    //var input0 = "abc";
    //var results = ["abc"];
    //shouldBe('regex668.exec(input0);', 'results');
    //var input1 = "abc\n";
    //var results = null;
    //shouldBe('regex668.exec(input1);', 'results');
    //
    //var regex670 = /\z(?<!\n)/;
    //var input0 = "abc";
    //var results = [""];
    //shouldBe('regex670.exec(input0);', 'results');
    //var input1 = "abc\n";
    //var results = null;
    //shouldBe('regex670.exec(input1);', 'results');

    // DISABLED:
    // These tests use \Q and \E tokens. These are not currently valid in ECMAScript,
    // but these tests may be useful if similar constructs are introduced in the future.

    //var regex617 = /abc\Qabc\Eabc/;
    //var input0 = "abcabcabc";
    //var results = ["abcabcabc"];
    //shouldBe('regex617.exec(input0);', 'results');
    //
    //var regex618 = /abc\Q(*+|\Eabc/;
    //var input0 = "abc(*+|abc";
    //var results = ["abc(*+|abc"];
    //shouldBe('regex618.exec(input0);', 'results');
    //
    //var regex619 = /\Qabc\$xyz\E/;
    //var input0 = "abc\\\$xyz";
    //var results = ["abc\\$xyz"];
    //shouldBe('regex619.exec(input0);', 'results');
    //
    //var regex620 = /\Qabc\E\$\Qxyz\E/;
    //var input0 = "abc\$xyz";
    //var results = ["abc$xyz"];
    //shouldBe('regex620.exec(input0);', 'results');
    //
    //var regex629 = /[z\Qa-d]\E]/;
    //var input0 = "z";
    //var results = ["z"];
    //shouldBe('regex629.exec(input0);', 'results');
    //var input1 = "a";
    //var results = ["a"];
    //shouldBe('regex629.exec(input1);', 'results');
    //var input2 = "-";
    //var results = ["-"];
    //shouldBe('regex629.exec(input2);', 'results');
    //var input3 = "d";
    //var results = ["d"];
    //shouldBe('regex629.exec(input3);', 'results');
    //var input4 = "]";
    //var results = ["]"];
    //shouldBe('regex629.exec(input4);', 'results');
    //// Failers
    //var input5 = "b";
    //var results = null;
    //shouldBe('regex629.exec(input5);', 'results');
    //
    //var regex653 = /^\Eabc/;
    //var input0 = "abc";
    //var results = ["abc"];
    //shouldBe('regex653.exec(input0);', 'results');
    //
    //var regex654 = /^[\Eabc]/;
    //var input0 = "a";
    //var results = ["a"];
    //shouldBe('regex654.exec(input0);', 'results');
    //// Failers
    //var input1 = "E";
    //var results = null;
    //shouldBe('regex654.exec(input1);', 'results');
    //
    //var regex655 = /^[a-\Ec]/;
    //var input0 = "b";
    //var results = ["b"];
    //shouldBe('regex655.exec(input0);', 'results');
    //// Failers
    //var input1 = "-";
    //var results = null;
    //shouldBe('regex655.exec(input1);', 'results');
    //var input2 = "E";
    //var results = null;
    //shouldBe('regex655.exec(input2);', 'results');
    //
    //var regex656 = /^[a\E\E-\Ec]/;
    //var input0 = "b";
    //var results = ["b"];
    //shouldBe('regex656.exec(input0);', 'results');
    //// Failers
    //var input1 = "-";
    //var results = null;
    //shouldBe('regex656.exec(input1);', 'results');
    //var input2 = "E";
    //var results = null;
    //shouldBe('regex656.exec(input2);', 'results');
    //
    //var regex657 = /^[\E\Qa\E-\Qz\E]+/;
    //var input0 = "b";
    //var results = ["b"];
    //shouldBe('regex657.exec(input0);', 'results');
    //// Failers
    //var input1 = "-";
    //var results = null;
    //shouldBe('regex657.exec(input1);', 'results');
    //
    //var regex658 = /^[a\Q]bc\E]/;
    //var input0 = "a";
    //var results = ["a"];
    //shouldBe('regex658.exec(input0);', 'results');
    //var input1 = "]";
    //var results = ["]"];
    //shouldBe('regex658.exec(input1);', 'results');
    //var input2 = "c";
    //var results = ["c"];
    //shouldBe('regex658.exec(input2);', 'results');
    //
    //var regex659 = /^[a-\Q\E]/;
    //var input0 = "a";
    //var results = ["a"];
    //shouldBe('regex659.exec(input0);', 'results');
    //var input1 = "-";
    //var results = ["-"];
    //shouldBe('regex659.exec(input1);', 'results');

    // DISABLED:
    // These tests use \A and \z tokens. These are not currently valid in ECMAScript,
    // but these tests may be useful if similar constructs are introduced in the future.

    //var regex591 = /b\z/;
    //var input0 = "a\nb";
    //var results = ["b"];
    //shouldBe('regex591.exec(input0);', 'results');
    //
    //var regex615 = /(?!\A)x/m;
    //var input0 = "a\nxb\n";
    //var results = ["x"];
    //shouldBe('regex615.exec(input0);', 'results');
});
