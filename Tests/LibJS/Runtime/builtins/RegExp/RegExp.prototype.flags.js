test("basic functionality", () => {
    expect(/foo/.flags).toBe("");
    expect(/foo/d.flags).toBe("d");
    expect(/foo/g.flags).toBe("g");
    expect(/foo/i.flags).toBe("i");
    expect(/foo/m.flags).toBe("m");
    expect(/foo/s.flags).toBe("s");
    expect(/foo/v.flags).toBe("v");
    expect(/foo/u.flags).toBe("u");
    expect(/foo/y.flags).toBe("y");
    // prettier-ignore
    expect(/foo/dsgimyu.flags).toBe("dgimsuy");
    // prettier-ignore
    expect(/foo/dgimsvy.flags).toBe("dgimsvy");
});
