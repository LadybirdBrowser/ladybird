test("cached environment calls keep throwing TDZ reference errors", () => {
    let calls = 0;

    expect(() => {
        {
            function g() {
                try {
                    return f();
                } catch (error) {
                    ++calls;
                    if (calls === 1) return g();
                    throw error;
                }
            }

            g();
            let f = 1;
        }
    }).toThrowWithMessage(ReferenceError, "Binding f is not initialized");

    expect(calls).toBe(2);
});
