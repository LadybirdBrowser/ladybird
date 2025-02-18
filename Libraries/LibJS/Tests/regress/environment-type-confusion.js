test("PR: #3620; Don't crash when running with UBSAN", () => {
    async function outer(a) {
        try {
            let [] = this;
        } catch(e17) {
            a.foo = a;
            try {
            } catch(e20) {
            }
        } finally {
            function inner() {
                a;
            }
            inner();
        }
    }

    outer();
    outer(outer);
});
