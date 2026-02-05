test(function() {
  var invalid = [
        ["A", "?>"],
        ["\u00B7A", "x"],
        ["\u00D7A", "x"],
        ["A\u00D7", "x"],
        ["\\A", "x"],
        ["\f", "x"],
        [0, "x"],
        ["0", "x"]
      ],
      valid = [
        ["xml:fail", "x"],
        ["A\u00B7A", "x"],
        ["a0", "x"]
      ]

  for (var i = 0, il = invalid.length; i < il; i++) {
    test(function() {
      assert_throws_dom("INVALID_CHARACTER_ERR", function() {
        document.createProcessingInstruction(invalid[i][0], invalid[i][1])
      })
    }, "Should throw an INVALID_CHARACTER_ERR for target " +
       format_value(invalid[i][0]) + " and data " +
       format_value(invalid[i][1]) + ".")
  }
  for (var i = 0, il = valid.length; i < il; ++i) {
    test(function() {
      var pi = document.createProcessingInstruction(valid[i][0], valid[i][1]);
      assert_equals(pi.target, valid[i][0]);
      assert_equals(pi.data, valid[i][1]);
      assert_equals(pi.ownerDocument, document);
      assert_true(pi instanceof ProcessingInstruction);
      assert_true(pi instanceof Node);
    }, "Should get a ProcessingInstruction for target " +
      format_value(valid[i][0]) + " and data " +
      format_value(valid[i][1]) + ".")
  }
})
