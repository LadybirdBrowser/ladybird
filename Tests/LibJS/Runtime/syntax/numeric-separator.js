describe("numeric separators", () => {
    test("numeric separator works for 'normal' number", () => {
        expect("1_2").toEvalTo(12);
        expect("4_2.4_2").toEvalTo(42.42);
        expect("1_2e0_2").toEvalTo(1200);

        expect("1_2E+_1").not.toEval();
        expect("1_2E+0_1").toEvalTo(120);
    });

    test("cannot use numeric separator after .", () => {
        expect("4._3").not.toEval();
        expect("0._3").not.toEval();
        expect("1_.3._3").not.toEval();

        // Actually a valid attempt to get property '_3' on 1.3 which fails but does parse.
        expect("1.3._3").toEval();
    });

    test("cannot use numeric separator in octal escaped number", () => {
        expect("00_1").not.toEval();
        expect("01_1").not.toEval();
        expect("07_3").not.toEval();
        expect("00_1").not.toEval();
    });
});
