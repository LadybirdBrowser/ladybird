// See also /fetch/api/response/json.any.js

async_test(t => {
  const xhr = new XMLHttpRequest();
  xhr.responseType = "json";
  xhr.open("GET", `data:,\uFEFF{ "b": 1, "a": 2, "b": 3 }`);
  xhr.send();
  xhr.onload = t.step_func_done(() => {
    assert_array_equals(Object.keys(xhr.response), ["b", "a"]);
    assert_equals(xhr.response.a, 2);
    assert_equals(xhr.response.b, 3);
  });
}, "Ensure the correct JSON parser is used");

async_test(t => {
  const client = new XMLHttpRequest();
  client.responseType = 'json';
  client.open("GET", "resources/utf16-bom.json");
  client.send();
  client.onload = t.step_func_done(() => {
    assert_equals(client.response, null);
  });
}, "Ensure UTF-16 results in an error");

// A JSON response is always decoded as UTF-8, whatever charset the response or
// overrideMimeType() declares. The response body is "é" encoded as UTF-8.
for (const charset of ["windows-1252", "utf-16le"]) {
  async_test(t => {
    const client = new XMLHttpRequest();
    client.responseType = "json";
    const body = new Uint8Array([0x22, 0xC3, 0xA9, 0x22]);
    client.open("GET", URL.createObjectURL(new Blob([body], { type: `application/json;charset=${charset}` })));
    client.send();
    client.onload = t.step_func_done(() => {
      assert_equals(client.response, "\u00e9");
    });
  }, `Ensure the response charset (${charset}) is ignored`);

  async_test(t => {
    const client = new XMLHttpRequest();
    client.responseType = "json";
    const body = new Uint8Array([0x22, 0xC3, 0xA9, 0x22]);
    client.open("GET", URL.createObjectURL(new Blob([body], { type: "application/json" })));
    client.overrideMimeType(`application/json;charset=${charset}`);
    client.send();
    client.onload = t.step_func_done(() => {
      assert_equals(client.response, "\u00e9");
    });
  }, `Ensure the overrideMimeType() charset (${charset}) is ignored`);
}
