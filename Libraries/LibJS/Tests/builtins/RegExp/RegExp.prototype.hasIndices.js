test("basic functionality", () => {
    const str = "foo bar foo";

    var regex = new RegExp("bar");
    expect(regex.hasIndices).toBeFalse();
    expect(regex.exec(str).indices).toBeUndefined();

    regex = new RegExp("foo", "gd");
    expect(regex.hasIndices).toBeTrue();
    expect(regex.exec(str).indices[0]).toEqual([0, 3]);
    expect(regex.exec(str).indices[0]).toEqual([8, 11]);

    regex = new RegExp("(foo)", "gd");
    expect(regex.hasIndices).toBeTrue();
    {
        var result = regex.exec(str).indices;
        expect(result.length).toBe(2);
        expect(result[0]).toEqual([0, 3]);
        expect(result[1]).toEqual([0, 3]);
        expect(result.groups).toBeUndefined();
    }
    {
        var result = regex.exec(str).indices;
        expect(result.length).toBe(2);
        expect(result[0]).toEqual([8, 11]);
        expect(result[1]).toEqual([8, 11]);
        expect(result.groups).toBeUndefined();
    }

    regex = new RegExp("(?<group_name>foo)", "gd");
    expect(regex.hasIndices).toBeTrue();
    {
        var result = regex.exec(str).indices;
        expect(result.length).toBe(2);
        expect(result[0]).toEqual([0, 3]);
        expect(result[1]).toEqual([0, 3]);
        expect(result.groups).toEqual({ group_name: [0, 3] });
    }
    {
        var result = regex.exec(str).indices;
        expect(result.length).toBe(2);
        expect(result[0]).toEqual([8, 11]);
        expect(result[1]).toEqual([8, 11]);
        expect(result.groups).toEqual({ group_name: [8, 11] });
    }

    regex = /(?<keyword>let|const|var)\s+(?<id>[a-zA-Z_$][0-9a-zA-Z_$]*)/d;
    expect(regex.hasIndices).toBeTrue();
    {
        var result = regex.exec("let foo").indices;
        expect(result.groups).toEqual({ keyword: [0, 3], id: [4, 7] });
    }
});
