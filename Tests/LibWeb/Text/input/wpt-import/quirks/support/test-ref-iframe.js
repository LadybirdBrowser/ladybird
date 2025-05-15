function setupIframe() {
  var iframe = document.querySelector('iframe');
  var html = "<style id=style></style><div id=test></div><div id=ref></div><svg><circle id=svg /><circle id=svg_ref /></svg>";
  if (iframe.className === "limited-quirks") {
    html = '<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN" "http://www.w3.org/TR/html4/loose.dtd">' + html;
  } else if (iframe.className === "no-quirks") {
    html = '<!DOCTYPE HTML>' + html;
  }
  window.quirks = iframe.className === "quirks";
  window.win = iframe.contentWindow;
  win.document.open();
  win.document.write(html);
  win.document.close();
  ['style', 'test', 'ref', 'svg', 'svg_ref'].forEach(function(id) {
      win[id] = win.document.getElementById(id);
  });
}
