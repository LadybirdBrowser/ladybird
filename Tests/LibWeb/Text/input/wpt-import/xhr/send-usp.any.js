const NUM_TESTS = 128;

function encode(n) {
  if (n === 0x20) {
    return "\x2B";
  }

  if (n === 0x2A || n === 0x2D || n === 0x2E ||
      (0x30 <= n && n <= 0x39) || (0x41 <= n && n <= 0x5A) ||
      n === 0x5F || (0x61 <= n && n <= 0x7A)) {
    return String.fromCharCode(n);
  }

  var s = n.toString(16).toUpperCase();
  return "%" + (s.length === 2 ? s : '0' + s);
}

  var tests = [];
  var overall_test = async_test("Overall fetch with URLSearchParams");
  for (var i = 0; i < NUM_TESTS; i++) {
    // Multiple subtests so that failures can be fine-grained
    tests[i] = async_test("XMLHttpRequest.send(URLSearchParams) (" + i + ")");
  }

  // We use a single XHR since this test tends to time out
  // with 128 consecutive fetches when run in parallel
  // with many other WPT tests.
  var x = new XMLHttpRequest();
  x.onload = overall_test.step_func(function() {
    var response_split = x.response.split("&");
    overall_test.done();
    for (var i = 0; i < NUM_TESTS; i++) {
      tests[i].step(function() {
        assert_equals(response_split[i], "a" + i + "="+encode(i));
        tests[i].done();
      });
    }
  });
  x.onerror = overall_test.unreached_func();

  x.open("POST", "resources/content.py");
  var usp = new URLSearchParams();
  for (var i = 0; i < NUM_TESTS; i++) {
    usp.append("a" + i, String.fromCharCode(i));
  }
  x.send(usp);

// Content-Type and request body handling for send(URLSearchParams).
// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
[
  {
    method: "POST",
    authorContentType: null,
    expectedContentType: "application/x-www-form-urlencoded;charset=UTF-8",
    expectedBody: "a=b",
    description: "POST uses the URLSearchParams MIME type when no Content-Type was set"
  },
  {
    method: "POST",
    authorContentType: "text/plain",
    expectedContentType: "text/plain",
    expectedBody: "a=b",
    description: "POST keeps an author-set Content-Type"
  },
  {
    // The spec only reconciles the charset parameter when the body is a Document or a string, but
    // Blink, Gecko and WebKit all do it for URLSearchParams too, and the body is always UTF-8 here.
    method: "POST",
    authorContentType: "text/plain;charset=windows-1252",
    expectedContentType: "text/plain;charset=UTF-8",
    expectedBody: "a=b",
    description: "POST replaces the charset parameter of an author-set Content-Type"
  },
  {
    method: "GET",
    authorContentType: null,
    expectedContentType: "NO",
    expectedBody: "",
    description: "GET sends neither a body nor a Content-Type"
  },
  {
    method: "HEAD",
    authorContentType: null,
    expectedContentType: "NO",
    expectedBody: null,
    description: "HEAD sends neither a body nor a Content-Type"
  },
  {
    method: "GET",
    authorContentType: "text/plain",
    expectedContentType: "text/plain",
    expectedBody: "",
    description: "GET keeps an author-set Content-Type"
  }
].forEach(({ method, authorContentType, expectedContentType, expectedBody, description }) => {
  promise_test(t => {
    const client = new XMLHttpRequest();
    client.open(method, "resources/content.py");
    if (authorContentType !== null)
      client.setRequestHeader("Content-Type", authorContentType);
    client.send(new URLSearchParams("a=b"));
    return new Promise((resolve, reject) => {
      client.onload = resolve;
      client.onerror = () => reject(new Error("Network error"));
    }).then(() => {
      assert_equals(client.getResponseHeader("X-Request-Method"), method, "request method");
      assert_equals(client.getResponseHeader("X-Request-Content-Type"), expectedContentType, "Content-Type request header");
      if (expectedBody !== null)
        assert_equals(client.response, expectedBody, "request body");
      if (expectedBody === "")
        assert_equals(client.getResponseHeader("X-Request-Content-Length"), "NO", "Content-Length request header");
    });
  }, `XMLHttpRequest.send(URLSearchParams): ${description}`);
});
