test("delete super[key] throws ReferenceError without calling ToPropertyKey", () => {
    var key = {
        toString() {
            throw new Error("ToPropertyKey performed");
        },
    };

    var obj = {
        m() {
            delete super[key];
        },
    };

    expect(() => obj.m()).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");
});

test("delete super.x throws ReferenceError", () => {
    var obj = {
        m() {
            delete super.x;
        },
    };

    expect(() => obj.m()).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");
});
