t.step(
function() {
  order.push(5);
  assert_equals(document.getElementsByTagName("meta").length, 0);
});
