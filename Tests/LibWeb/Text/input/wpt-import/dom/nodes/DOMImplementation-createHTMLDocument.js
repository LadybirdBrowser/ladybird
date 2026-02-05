function createHTMLDocuments(checkDoc) {
  var tests = [
    ["", "", ""],
    [null, "null", "null"],
    [undefined, undefined, ""],
    ["foo  bar baz", "foo  bar baz", "foo bar baz"],
    ["foo\t\tbar baz", "foo\t\tbar baz", "foo bar baz"],
    ["foo\n\nbar baz", "foo\n\nbar baz", "foo bar baz"],
    ["foo\f\fbar baz", "foo\f\fbar baz", "foo bar baz"],
    ["foo\r\rbar baz", "foo\r\rbar baz", "foo bar baz"],
  ]

  tests.forEach(function(t, i) {
    var title = t[0], expectedtitle = t[1], normalizedtitle = t[2]
    test(function() {
      var doc = document.implementation.createHTMLDocument(title);
      checkDoc(doc, expectedtitle, normalizedtitle)
    }, "createHTMLDocument test " + i + ": " + t.map(function(el) { return format_value(el) }))
  })

  test(function() {
    var doc = document.implementation.createHTMLDocument();
    checkDoc(doc, undefined, "")
  }, "Missing title argument");
}
