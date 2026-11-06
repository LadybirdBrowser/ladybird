test("the initializer of a let in a loop sees it in its dead zone in every iteration", () => {
    function run() {
        const results = [];
        for (let i = 0; i < 2; ++i) {
            try {
                if (i === 0) continue;
                let value = value;
                results.push(value);
            } catch (error) {
                results.push(error.name);
            }
        }
        for (let i = 0; i < 2; ++i) {
            try {
                let value = i === 0 ? 1 : value;
                results.push(value);
            } catch (error) {
                results.push(error.name);
            }
        }
        return results;
    }
    expect(run()).toEqual(["ReferenceError", 1, "ReferenceError"]);
});
