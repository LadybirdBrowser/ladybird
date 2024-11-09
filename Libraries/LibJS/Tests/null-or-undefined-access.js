test("null/undefined object", () => {
    [null, undefined].forEach(value => {
        let foo = value;

        expect(() => {
            foo.bar;
        }).toThrowWithMessage(TypeError, `Cannot access property "bar" on ${value} object "foo"`);

        expect(() => {
            foo.bar = 1;
        }).toThrowWithMessage(TypeError, `Cannot access property "bar" on ${value} object "foo"`);

        expect(() => {
            foo[0];
        }).toThrowWithMessage(TypeError, `Cannot access property "0" on ${value} object "foo"`);

        expect(() => {
            foo[0] = 1;
        }).toThrowWithMessage(TypeError, `Cannot access property "0" on ${value} object "foo"`);
    });
});

test("null/undefined object key", () => {
    [null, undefined].forEach(value => {
        let foo = { bar: value };

        expect(() => {
            foo.bar.baz;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "baz" on ${value} object "foo.bar"`
        );

        expect(() => {
            foo.bar.baz = 1;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "baz" on ${value} object "foo.bar"`
        );

        expect(() => {
            foo["bar"].baz;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "baz" on ${value} object "foo['bar']"`
        );

        expect(() => {
            foo["bar"].baz = 1;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "baz" on ${value} object "foo['bar']"`
        );
    });
});

test("null/undefined array index", () => {
    [null, undefined].forEach(value => {
        let foo = [value];
        let index = 0;

        expect(() => {
            foo[0].bar;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "bar" on ${value} object "foo[0]"`
        );

        expect(() => {
            foo[0].bar = 1;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "bar" on ${value} object "foo[0]"`
        );

        expect(() => {
            foo[index].bar;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "bar" on ${value} object "foo[index]"`
        );

        expect(() => {
            foo[index].bar = 1;
        }).toThrowWithMessage(
            TypeError,
            `Cannot access property "bar" on ${value} object "foo[index]"`
        );
    });
});
