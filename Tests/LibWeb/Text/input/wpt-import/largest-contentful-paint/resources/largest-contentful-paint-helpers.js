const image_delay = 2000;
const delay_pipe_value = image_delay / 1000;

const await_with_timeout = async (delay, message, promise, cleanup = ()=>{}) => {
  let timeout_id;
  const timeout = new Promise((_, reject) => {
    timeout_id = step_timeout(() =>
      reject(new DOMException(message, "TimeoutError")), delay)
  });
  let result = null;
  try {
    result = await Promise.race([promise, timeout]);
    clearTimeout(timeout_id);
  } finally {
    cleanup();
  }
  return result;
};

// Receives an image LargestContentfulPaint |entry| and checks |entry|'s attribute values.
// The |timeLowerBound| parameter is a lower bound on the loadTime value of the entry.
// The |options| parameter may contain some string values specifying the following:
// * 'renderTimeIs0': the renderTime should be 0 (image does not pass Timing-Allow-Origin checks).
//     When not present, the renderTime should not be 0 (image passes the checks).
// * 'sizeLowerBound': the |expectedSize| is only a lower bound on the size attribute value.
//     When not present, |expectedSize| must be exactly equal to the size attribute value.
// * 'approximateSize': the |expectedSize| is only approximate to the size attribute value.
//     This option is mutually exclusive to 'sizeLowerBound'.
function checkImage(entry, expectedUrl, expectedID, expectedSize, timeLowerBound, options = []) {
  assert_equals(entry.name, '', "Entry name should be the empty string");
  assert_equals(entry.entryType, 'largest-contentful-paint',
    "Entry type should be largest-contentful-paint");
  assert_equals(entry.duration, 0, "Entry duration should be 0");
  // The entry's url can be truncated.
  assert_equals(expectedUrl.substr(0, 100), entry.url.substr(0, 100),
    `Expected URL ${expectedUrl} should at least start with the entry's URL ${entry.url}`);
  assert_equals(entry.id, expectedID, "Entry ID matches expected one");
  assert_equals(entry.element, document.getElementById(expectedID),
    "Entry element is expected one");
  if (options.includes('skip')) {
    return;
  }
  assert_greater_than_equal(performance.now(), entry.renderTime,
    'renderTime should occur before the entry is dispatched to the observer.');
  assert_approx_equals(entry.startTime, entry.renderTime, 0.001,
    'startTime should be equal to renderTime to the precision of 1 millisecond.');
  if (options.includes('sizeLowerBound')) {
    assert_greater_than(entry.size, expectedSize);
  } else if (options.includes('approximateSize')) {
    assert_approx_equals(entry.size, expectedSize, 1);
  } else{
    assert_equals(entry.size, expectedSize);
  }

  assert_greater_than_equal(entry.paintTime, timeLowerBound, 'paintTime should represent the time when the UA started painting');

  // PaintTimingMixin
  if ("presentationTime" in entry && entry.presentationTime !== null) {
    assert_greater_than(entry.presentationTime, entry.paintTime);
    assert_equals(entry.presentationTime, entry.renderTime);
  } else {
    assert_equals(entry.renderTime, entry.paintTime);
  }

  if (options.includes('animated')) {
    assert_less_than(entry.renderTime, image_delay,
      'renderTime should be smaller than the delay applied to the second frame');
    assert_greater_than(entry.renderTime, 0,
      'renderTime should be larger than 0');
  }
  else {
    assert_between_inclusive(entry.loadTime, timeLowerBound, entry.renderTime,
      'loadTime should occur between the lower bound and the renderTime');
  }
}

const load_and_observe = url => {
  return new Promise(resolve => {
    (new PerformanceObserver(entryList => {
      for (let entry of entryList.getEntries()) {
        if (entry.url == url) {
          resolve(entryList.getEntries()[0]);
        }
      }
    })).observe({ type: 'largest-contentful-paint', buffered: true });
    const img = new Image();
    img.id = 'image_id';
    img.src = url;
    document.body.appendChild(img);
  });
};

const load_video_and_observe = url => {
  return new Promise(resolve => {
    (new PerformanceObserver(entryList => {
      for (let entry of entryList.getEntries()) {
        if (entry.url == url) {
          resolve(entryList.getEntries()[0]);
        }
      }
    })).observe({ type: 'largest-contentful-paint', buffered: true });
    const video = document.createElement("video");
    video.id = 'video_id';
    video.src = url;
    video.autoplay = true;
    video.muted = true;
    video.loop = true;
    document.body.appendChild(video);
  });
};

const getLCPStartTime = (identifier) => {
  return new Promise(resolve => {
    new PerformanceObserver((entryList, observer) => {
      entryList.getEntries().forEach(e => {
        if (e.url.includes(identifier)) {
          resolve(e);
          observer.disconnect();
        }
      });
    }).observe({ type: 'largest-contentful-paint', buffered: true });
  });
}

const getFCPStartTime = () => {
  return performance.getEntriesByName('first-contentful-paint')[0];
}

const add_text = (text) => {
  const paragraph = document.createElement('p');
  paragraph.innerHTML = text;
  document.body.appendChild(paragraph);
}

const loadImage = (url, shouldBeIgnoredForLCP = false) => {
  return new Promise(function (resolve, reject) {
    let image = document.createElement('img');
    image.addEventListener('load', () => { resolve(image); });
    image.addEventListener('error', reject);
    image.src = url;
    if (shouldBeIgnoredForLCP)
      image.style.opacity = 0;
    document.body.appendChild(image);
  });
}

const raf = () => {
  return new Promise(resolve => requestAnimationFrame(resolve));
}
