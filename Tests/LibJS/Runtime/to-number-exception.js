const message = "oops, Value::to_number() failed";

const o = {
    toString() {
        throw new Error(message);
    },
};

test("basic functionality", () => {
    expect(() => {
        +o;
    }).toThrowWithMessage(Error, message);

    expect(() => {
        o - 1;
    }).toThrowWithMessage(Error, message);

    expect(() => {
        "foo".charAt(o);
    }).toThrowWithMessage(Error, message);

    expect(() => {
        "bar".repeat(o);
    }).toThrowWithMessage(Error, message);
});
