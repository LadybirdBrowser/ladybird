function downloadReferrerTest(tagName, attributes, expectedReferrer, description) {
  promise_test(async t => {
    const id = token();
    const element = document.createElement(tagName);
    element.href = showRequestHeaders(location.origin, id);
    element.download = "";
    for (const [name, value] of Object.entries(attributes))
      element.setAttribute(name, value);
    document.body.append(element);
    t.add_cleanup(() => element.remove());

    element.click();

    const headers = JSON.parse(await receive(id));
    assert_equals(headers.referer ?? "", expectedReferrer, "Referer header of the download request");
  }, description);
}
