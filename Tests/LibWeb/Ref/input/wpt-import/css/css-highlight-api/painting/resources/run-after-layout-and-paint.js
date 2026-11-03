// This is inspired in runAfterLayoutAndPaint() from
// third_party/blink/web_tests/resources/run-after-layout-and-paint.js.
function runAfterLayoutAndPaint(callback) {
  // See http://crrev.com/c/1395193/10/third_party/blink/web_tests/http/tests/resources/run-after-layout-and-paint.js
  // for more discussions.
  requestAnimationFrame(function() {
    requestAnimationFrame(function() {
      callback();
    });
  });
}