test("overwriting this during function call still binds the original", () => {
    let tmp = new Map();
    // prettier-ignore
    tmp.set("", tmp = []);
});
