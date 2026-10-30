var fakeCustomData = (function() {
  var buffer = new ArrayBuffer(2);
  new DataView(buffer).setInt16(0, 42, true);
  var canvas = new OffscreenCanvas(100, 100);
  var context = canvas.getContext("2d");

  var map = new Map();
  var set = new Set();
  map.set("test", 42);
  set.add(4);
  set.add(2);

  return {
    primitives: {
      a: 123,
      b: "test",
      c: true,
      d: [1, 2, 3],
    },
    date: new Date(2013, 2, 1, 1, 10),
    regexp: new RegExp("[^.]+"),
    arrayBuffer: buffer,
    imageData: context.createImageData(100, 100),
    map,
    set,
  };
})();

function assert_custom_data(dataObj) {
  assert_equals(typeof dataObj, "object", "data should be a JS object");
  assert_equals(
    JSON.stringify(dataObj.primitives),
    JSON.stringify(fakeCustomData.primitives),
    "data.primitives should be preserved"
  );
  assert_equals(
    dataObj.date.toDateString(),
    fakeCustomData.date.toDateString(),
    "data.date should be preserved"
  );
  assert_equals(
    dataObj.regexp.exec("http://www.domain.com")[0].substr(7),
    "www",
    "data.regexp should be preserved"
  );
  assert_equals(
    new Int16Array(dataObj.arrayBuffer)[0],
    42,
    "data.arrayBuffer should be preserved"
  );
  assert_equals(
    JSON.stringify(dataObj.imageData.data),
    JSON.stringify(fakeCustomData.imageData.data),
    "data.imageData should be preserved"
  )
  assert_equals(
    dataObj.map.get("test"),
    42,
    "data.map should be preserved"
  );
  assert_true(
    dataObj.set.has(4) && dataObj.set.has(2),
    "data.set should be preserved"
  );
}
