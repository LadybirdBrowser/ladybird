// https://github.com/LadybirdBrowser/ladybird/issues/3622
// A nested function declared in an async function's "finally" block reads a parameter whose binding we used to look up
// through a mis-typed environment coordinate cache, leading to an OOB read. The binding must resolve to the real value.
test("binding lookup through a nested async finally function does not confuse environment types", () => {
    async function f0(a1) {
        try {
            let [] = this;
        } catch (e17) {
            a1.h = a1;
            try {
            } catch (e20) {}
        } finally {
            function f21() {
                a1;
            }
            f21();
        }
    }

    f0().catch(() => {});
    f0(f0).catch(() => {});

    // f0(f0) takes the catch path and runs a1.h = a1 with a1 === f0. A mis-typed environment lookup would store garbage
    // instead of f0 here (or crash).
    expect(f0.h).toBe(f0);
});
