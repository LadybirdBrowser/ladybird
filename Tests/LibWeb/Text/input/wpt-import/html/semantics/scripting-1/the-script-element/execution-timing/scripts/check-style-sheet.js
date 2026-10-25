test(function() {
  assert_equals(getComputedStyle(document.getElementById("test")).position,
                "fixed");
});
