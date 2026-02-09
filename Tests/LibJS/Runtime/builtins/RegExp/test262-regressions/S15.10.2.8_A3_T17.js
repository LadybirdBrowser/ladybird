// https://github.com/tc39/test262/blob/main/test/built-ins/RegExp/S15.10.2.8_A3_T17.js
describe("S15.10.2.8_A3_T17", () => {
    test("nested capture groups", () => {
        let __body = "";
        __body += '<body onXXX="alert(event.type);">\n';
        __body += "<p>Kibology for all<\/p>\n";
        __body += "<p>All for Kibology<\/p>\n";
        __body += "<\/body>";

        let __html = "";
        __html += "<html>\n";
        __html += __body;
        __html += "\n<\/html>";

        const __executed = /<body.*>((.*\n?)*?)<\/body>/i.exec(__html);

        const __expected = [
            __body,
            "\n<p>Kibology for all</p>\n<p>All for Kibology</p>\n",
            "<p>All for Kibology</p>\n",
        ];
        __expected.index = 7;
        __expected.input = __html;

        expect(__executed.length).toBe(__expected.length);
        expect(__executed.index).toBe(__expected.index);
        expect(__executed.input).toBe(__expected.input);

        for (let index = 0; index < __expected.length; index++) {
            expect(__executed[index]).toBe(__expected[index]);
        }
    });
});
