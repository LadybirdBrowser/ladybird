async_test(test => {
  const client = new XMLHttpRequest();
  const data = "This is 22 bytes long.";
  let happened = false;
  client.upload.onprogress = test.step_func(e => {
    assert_true(e.lengthComputable);
    assert_equals(e.total, data.length);
    happened = true;
  });
  client.onload = test.step_func_done(() => {
    assert_true(happened);
    assert_true(client.responseText.includes(`Content-Length: ${data.length}`));
  });
  client.onerror = test.unreached_func();
  client.open("POST", "resources/echo-headers.py");
  client.send(data);
}, "Uploads need to set the Content-Length header");

async_test(test => {
  const client = new XMLHttpRequest();
  const data = "blah";
  const url = URL.createObjectURL(new Blob([data]));
  client.open("GET", url);
  client.send();
  client.onload = test.step_func_done(e => {
    assert_true(e.lengthComputable);
    assert_equals(e.total, data.length);
    assert_equals(e.loaded, data.length);
    assert_equals(client.responseText, data);
    assert_equals(client.getResponseHeader("Content-Length"), String(data.length));
  });
}, "Fetched blob: URLs set the Content-Length header");
