test("trim", () => {
    expect(String.prototype.trim).toHaveLength(0);

    expect("   hello friends  ".trim()).toBe("hello friends");
    expect("hello friends   ".trim()).toBe("hello friends");
    expect("   hello friends".trim()).toBe("hello friends");

    expect("\thello friends\t".trim()).toBe("hello friends");
    expect("\thello friends".trim()).toBe("hello friends");
    expect("hello friends\t".trim()).toBe("hello friends");

    expect("\rhello friends\r".trim()).toBe("hello friends");
    expect("\rhello friends".trim()).toBe("hello friends");
    expect("hello friends\r".trim()).toBe("hello friends");

    expect("\rhello friends\n".trim()).toBe("hello friends");
    expect("\r\thello friends".trim()).toBe("hello friends");
    expect("hello friends\r\n".trim()).toBe("hello friends");
    expect("  \thello friends\r\n".trim()).toBe("hello friends");
    expect("\n\t\thello friends\r\n".trim()).toBe("hello friends");
    expect("\n\t\thello friends\t\t".trim()).toBe("hello friends");
});

test("trimStart", () => {
    expect(String.prototype.trimStart).toHaveLength(0);

    expect("   hello friends".trimStart()).toBe("hello friends");
    expect("hello friends   ".trimStart()).toBe("hello friends   ");
    expect("    hello friends   ".trimStart()).toBe("hello friends   ");

    expect("\thello friends".trimStart()).toBe("hello friends");
    expect("hello friends\t".trimStart()).toBe("hello friends\t");
    expect("\thello friends\t".trimStart()).toBe("hello friends\t");

    expect("\rhello friends".trimStart()).toBe("hello friends");
    expect("hello friends\r".trimStart()).toBe("hello friends\r");
    expect("\rhello friends\r".trimStart()).toBe("hello friends\r");
});

test("trimEnd", () => {
    expect(String.prototype.trimEnd).toHaveLength(0);

    expect("hello friends   ".trimEnd()).toBe("hello friends");
    expect("   hello friends".trimEnd()).toBe("   hello friends");
    expect("   hello friends   ".trimEnd()).toBe("   hello friends");

    expect("hello friends\t".trimEnd()).toBe("hello friends");
    expect("\thello friends".trimEnd()).toBe("\thello friends");
    expect("\thello friends\t".trimEnd()).toBe("\thello friends");

    expect("hello friends\r".trimEnd()).toBe("hello friends");
    expect("\rhello friends".trimEnd()).toBe("\rhello friends");
    expect("\rhello friends\r".trimEnd()).toBe("\rhello friends");

    expect("hello friends\n".trimEnd()).toBe("hello friends");
    expect("\r\nhello friends".trimEnd()).toBe("\r\nhello friends");
    expect("\rhello friends\r\n".trimEnd()).toBe("\rhello friends");
});

test("multi-byte code point", () => {
    expect("_\u180E".trim()).toBe("_\u180E");
    expect("\u180E".trim()).toBe("\u180E");
    expect("\u180E_".trim()).toBe("\u180E_");

    expect("_😀".trim()).toBe("_😀");
    expect("😀".trim()).toBe("😀");
    expect("😀_".trim()).toBe("😀_");
});
