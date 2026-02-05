function testConstants(objects, constants, msg) {
  objects.forEach(function(arr) {
    var o = arr[0], desc = arr[1];
    test(function() {
      constants.forEach(function(d) {
        assert_true(d[0] in o, "Object " + o + " doesn't have " + d[0])
        assert_equals(o[d[0]], d[1], "Object " + o + " value for " + d[0] + " is wrong")
      })
    }, "Constants for " + msg + " on " + desc + ".")
  })
}
