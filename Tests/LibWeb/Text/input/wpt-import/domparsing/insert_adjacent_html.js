function testThrowingNoParent(element, desc) {
  test(function() {
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("afterend", "") }
    );
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("beforebegin", "") }
    );
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("afterend", "foo") }
    );
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("beforebegin", "foo") }
    );
  }, "When the parent node is " + desc + ", insertAdjacentHTML should throw for beforebegin and afterend (text)");
  test(function() {
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("afterend", "<!-- fail -->") }
    );
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("beforebegin", "<!-- fail -->") }
    );
  }, "When the parent node is " + desc + ", insertAdjacentHTML should throw for beforebegin and afterend (comments)");
  test(function() {
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("afterend", "<div></div>") }
    );
    assert_throws_dom("NO_MODIFICATION_ALLOWED_ERR",
      function() { element.insertAdjacentHTML("beforebegin", "<div></div>") }
    );
  }, "When the parent node is " + desc + ", insertAdjacentHTML should throw for beforebegin and afterend (elements)");
}

