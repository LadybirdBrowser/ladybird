test("strict mode does not apply global object to |this|", () => {
    "use strict";
    let functionThis;
    (function () {
        functionThis = this;
    })();
    expect(functionThis).toBeUndefined();
});
