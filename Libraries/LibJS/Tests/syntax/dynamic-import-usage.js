describe("parsing", () => {
    test("can parse call import call", () => {
        expect("import('a')").toEval();
        expect("import('a', )").toEval();
        expect("import('a', {options: true})").toEval();
    });

    test("does not crash on unexpected tokens after import", () => {
        expect("f = import('a')").toEval();

        expect("f= import").not.toEval();
        expect("f= import;").not.toEval();
        expect("f= import?").not.toEval();
        expect("f= import'").not.toEval();
        expect("f= import 'a'").not.toEval();
        expect("f= import['a']").not.toEval();
    });
});
