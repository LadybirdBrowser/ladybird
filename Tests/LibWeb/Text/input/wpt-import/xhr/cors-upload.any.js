// META: title=Cross-Origin POST with preflight and FormData body should send body
// META: script=/common/get-host-info.sub.js
"use strict";

function testCorsFormDataUpload(description, path, method, form, headers, withCredentials) {
  const test = async_test(description);
  const client = new XMLHttpRequest();
  const url = corsURL(path);

  client.open(method, url, true);
  client.withCredentials = withCredentials;
  for (const key of Object.keys(headers)) {
    client.setRequestHeader(key, headers[key]);
  }

  client.send(form);

  client.onload = () => {
    test.step(() => {
      assert_equals(client.status, 200);
      assert_regexp_match(client.responseText, /Content-Disposition: form-data/);

      for (const key of form.keys()) {
        assert_regexp_match(client.responseText, new RegExp(key));
        assert_regexp_match(client.responseText, new RegExp(form.get(key)));
      }
    });
    test.done();
  };
}

function corsURL(path) {
  const url = new URL(path, location.href);
  url.hostname = get_host_info().REMOTE_HOST;
  return url.href;
}

const form = new FormData();
form.append("key", "value");

testCorsFormDataUpload(
  "Cross-Origin POST FormData body but no preflight",
  "resources/echo-content-cors.py",
  "POST",
  form,
  {},
  false
);

testCorsFormDataUpload(
  "Cross-Origin POST with preflight and FormData body",
  "resources/echo-content-cors.py",
  "POST",
  form,
  {
    Authorization: "Bearer access-token"
  },
  true
);
