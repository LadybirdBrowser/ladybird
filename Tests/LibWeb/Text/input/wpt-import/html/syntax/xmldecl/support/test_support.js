function runAllTests() {
    let divs = document.getElementsByTagName("div");
    for (let i = 0; i < divs.length; ++i) {
        let div = divs[i];
        let expectation = div.className;
        let iframes = div.getElementsByTagName("iframe");
        for (let j = 0; j < iframes.length; ++j) {
            let iframe = iframes[j];
            let src = iframe.src;
            let doc = iframe.contentWindow.document;
            test(function() {
                assert_equals(doc.characterSet, expectation, 'Check');
                if (expectation == "windows-1251" || expectation == "windows-1252" && !(src.endsWith("/XML.htm") || src.endsWith("/XML-trail.htm"))) {
                    let fc = doc.firstChild;
                    switch (fc.nodeType) {
                        case Node.COMMENT_NODE:
                            assert_true(fc.nodeValue.startsWith("?xml"), 'Should start with ?xml');
                            break;
                        case Node.PROCESSING_INSTRUCTION_NODE:
                            assert_true(fc.target.startsWith("xml"), 'Should start with xml');
                            break;
                        default:
                            assert(false, "Should start with xml-* processing instruction, or an xml/xml-stylesheet comment")
                    }
                } else if (expectation == "UTF-16BE" || expectation == "UTF-16LE") {
                    let fc = doc.firstChild;
                    assert_equals(fc.nodeType, Node.COMMENT_NODE, 'Should have comment node');
                    assert_true(fc.nodeValue.startsWith("?x"), 'Should start with ?x');
                }
            }, "Check encoding " + expectation + ", " + src.substring(src.lastIndexOf("/") + 1));
        }
    }
}

