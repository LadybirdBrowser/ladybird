/* Taken from
   https://raw.github.com/WebKit/webkit/master/LayoutTests/webaudio/resources/buffer-loader.js */

function BufferLoader(context, urlList, callback) {
  this.context = context;
  this.urlList = urlList;
  this.onload = callback;
  this.bufferList = new Array();
  this.loadCount = 0;
}

BufferLoader.prototype.loadBuffer = function(url, index) {
  // Load buffer asynchronously
  var request = new XMLHttpRequest();
  request.open("GET", url, true);
  request.responseType = "arraybuffer";

  var loader = this;

  request.onload = function() {
    loader.context.decodeAudioData(request.response, decodeSuccessCallback, decodeErrorCallback);
  };

  request.onerror = function() {
    alert('BufferLoader: XHR error');
  };

  var decodeSuccessCallback = function(buffer) {
    loader.bufferList[index] = buffer;
    if (++loader.loadCount == loader.urlList.length)
      loader.onload(loader.bufferList);
  };

  var decodeErrorCallback = function() {
    alert('decodeErrorCallback: decode error');
  };

  request.send();
}

BufferLoader.prototype.load = function() {
  for (var i = 0; i < this.urlList.length; ++i)
    this.loadBuffer(this.urlList[i], i);
}
