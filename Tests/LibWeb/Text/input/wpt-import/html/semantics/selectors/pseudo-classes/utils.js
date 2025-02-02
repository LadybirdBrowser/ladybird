function getElementsByIds(ids) {
  var result = [];
  ids.forEach(function(id) {
    result.push(document.getElementById(id));
  });
  return result;
}

function testSelectorIdsMatch(selector, ids, testName) {
  test(function(){
    var elements = document.querySelectorAll(selector);
    assert_array_equals([...elements], getElementsByIds(ids));
  }, testName);
}

function testSelectorElementsMatch(selector, elements, testName) {
  test(function(){
    assert_array_equals([...document.querySelectorAll(selector)], elements);
  }, testName);
}
