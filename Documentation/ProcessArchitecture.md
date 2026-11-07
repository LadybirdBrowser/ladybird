# Ladybird process architecture

Ladybird uses a multi-process architecture to improve stability and security in the face of arbitrary (and possibly hostile) web content. Web content runs in sandboxed processes, and work that handles untrusted data, such as parsing images, is split off into further sandboxed helpers. A helper that crashes takes down as little as possible, and a compromised one reaches as little as possible.

## Process overview

```
                                 Browser
                                    |
     +----------+----------+--------+-------+----------------+
     |          |          |                |                |
 Compositor  WebContent  WebWorker    RequestServer    ImageDecoder
             (renderer)  (renderer)   (per site)       (per site)
                 |          |
     +-----------+          |
     |           |          |
 WebRTCClient    +----+-----+
                      |
             +--------+--------+
             |                 |
        MediaServer       AudioServer
             |
     VideoDecoderServer
```

**RequestServer** and **ImageDecoder** are shared by the renderers of a site, while the processes below the renderers each work for a single renderer. **Browser** launches all of them, because the sandboxes keep the others from spawning anything. It then connects them directly to each other, so that a renderer talks to its **RequestServer** or **ImageDecoder** without going through **Browser**.

| Process | How many | Launched |
| --- | --- | --- |
| **Browser** | One | By the user |
| **Compositor** | One | At startup, and again if it crashes |
| **WebContent** | One per set of documents that must share a process, see [site isolation](#site-isolation) | When a document needs one, with a spare kept ready for the next tab |
| **WebWorker** | One per worker | When a page or worker starts a worker |
| **RequestServer** | One per browsing session and top-level site, plus one per session with no site | The default session's one with no site at startup, the others on first use |
| **ImageDecoder** | One per browsing session and site, or one for a single process | When a renderer first has an image to decode |
| **MediaServer** | One per renderer | When the renderer first needs one |
| **AudioServer** | One per renderer | When the renderer first needs one |
| **VideoDecoderServer** | One per MediaServer | When its **MediaServer** asks for one |
| **WebRTCClient** | One per **WebContent** process | When the renderer first needs one |

On macOS, the kernel does not end a process when its parent dies, so a small **ProcessReaper** helper ends the others when **Browser** is gone.

### Process: Browser

This is the application itself, and it is not sandboxed. It shows the user interface and owns the state that must outlive or be shared between renderers: cookies, web storage, blob URLs, permissions, browsing history and the fonts of the system. Other processes ask **Browser** for these over IPC.

**Browser** also keeps its own record of every tab's navigables and documents, and decides which process hosts each document.

### Process: Compositor

This process turns what renderers record into pixels. Renderers send it display lists, which it rasterizes and presents, along with their canvases, WebGL contexts and video frames. It does the GPU work of rendering, so renderers need no access to the GPU.

It also runs animated images and scrolling, which keeps them moving while a renderer's main thread is busy. For animated images, it gets a connection of its own to the renderer's **ImageDecoder**.

If the **Compositor** crashes, **Browser** launches a new one and reconnects the renderers to it.

### Process: WebContent

This process hosts the main HTML/CSS engine (**LibWeb**) and runs JavaScript (**LibJS**). It gets input events from **Browser** and records its pages for the **Compositor**. It reaches the network only through **RequestServer**, and local files only through **Browser**.

A process holds one page for each tab it has documents in.

### Process: WebWorker

This process runs a single dedicated or shared worker. A worker runs in a process of its own, so a top-level site may only run so many of them at once.

### Process: RequestServer

This process uses networking protocols (like HTTP or HTTPS) to make requests on behalf of renderers, and resolves DNS names itself. It asks **Browser** for the cookies of each request, and hands **Browser** the cookies it receives.

Each top-level site has a **RequestServer** of its own in each browsing session, which only makes and caches requests for that site's partitions. Its sandbox confines its cache to that site's directory, and the process rejects requests that a renderer is not bound to make. A session's **RequestServer** with no site serves **Browser** itself and requests made under no site. A site's **RequestServer** ends once it has been idle for a while.

### Process: ImageDecoder

This process decodes images (PNG, JPEG, BMP, ICO, PBM, etc.) into bitmaps. It is strongly sandboxed, and can do little more than receive encoded image data and return bitmaps to its clients. It decodes into shared memory, which it hands to its clients as it is.

A decoder sees the images of every process it serves, so a malicious image that takes one over gets at those images. With site isolation, the processes of a site share an **ImageDecoder**, because they hold nothing of any other site's that they could not fetch for themselves. The site is that of the documents a process hosts, not their top-level site, so a frame never shares a decoder with the site embedding it, unless it has an opaque origin, which keeps it in the process of the document that made it. Private browsing is kept apart. Any other process, such as one with no site, gets a decoder of its own, and a process that comes to host a second site leaves the decoder of the first.

**Browser** launches a decoder when a process first has an image to decode, and ends it once the last process it serves is gone. A process that loses its decoder to a crash asks for a new one when it next has an image to decode.

### Processes: MediaServer, AudioServer and VideoDecoderServer

**MediaServer** plays the media of a renderer. Its video decoders run in a **VideoDecoderServer**, which **Browser** launches on its request. **AudioServer** talks to the audio devices of the system, for both a renderer's **MediaServer** and its WebAudio output.

### Process: WebRTCClient

This process runs the WebRTC connections of a **WebContent** process, outside of the renderer. It exits once that renderer's connection to it is gone.

## Site isolation

A renderer may be taken over by a page it hosts, so documents of different sites are kept in different processes. The `--site-isolation` option selects how far this goes:

- `iframe`, the default, isolates every document, including cross-site frames.
- `top-level` isolates top-level documents, and frames stay in the process of their parent.
- `disable` keeps every document in the process holding its navigable.

Documents that can script each other synchronously belong to one agent, and an agent runs in one process. A document with an opaque origin goes with the document that created it. Local file content and WebUI documents are kept in processes of their own, because those processes hold privileges that a website must not get.

`CanonicalNavigable::process_to_host()` in **LibWebView** makes this decision.

## How processes are spawned

`WebView::Process` spawns every helper. It creates the IPC transport first, gives the helper its end, and passes it only the environment variables that the helper needs. On Linux, the transport is a socket pair, and the helper finds its end through the `SOCKET_TAKEOVER` environment variable. On macOS, it is a pair of Mach ports, which the helper fetches from **Browser** by name. On Windows, it is a pipe, along with a connection to a handle broker in **Browser**, since a pipe cannot carry handles.

Once two processes need to talk, **Browser** creates a channel and hands an end to each of them as an `IPC::TransportHandle`. Helpers that serve several clients, such as **RequestServer** and **ImageDecoder**, have a separate control connection that only **Browser** speaks, which it uses to connect new clients.

A renderer that needs a helper it does not have yet asks **Browser** for a connection to it over IPC.

## Sandboxing

Every process in the table above except **Browser** applies a sandbox to itself early in its startup, unless **Browser** was started with `--disable-sandbox`. Each one's sandbox allows only what that process needs, and can be found in the `Sandbox` files in its directory under `Services`. **WebContent** and **WebWorker** share theirs, in `Services/RendererSandbox.h`.

On Linux, sandboxes are built from seccomp and Landlock. On macOS, they use the Seatbelt sandbox. Windows has no sandbox yet.

## Class overview

```
Browser process

    WebView::ViewImplementation        The view showing a tab, such as Qt's WebContentView
      WebView::WebContentPage          A page in a WebContent process
        WebView::WebContentClient      The client side of the IPC protocol, one per process

  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

WebContent process

    WebContent::ConnectionFromClient   The server side of the IPC protocol
      WebContent::PageHost             Holds the pages of the process
        WebContent::PageClient         Connects a page to the rest of the process

  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

LibWeb

    Web::Page
      Web::HTML::Navigable             A tab's top-level traversable, and a navigable for each frame
        Web::DOM::Document             The active document of a navigable, the root of the DOM tree
```

In the **Browser** process, each kind of view derives from `WebView::ViewImplementation`, which shows a tab. A tab's documents may be spread over several processes, so a view reaches each of them through a `WebView::WebContentPage`, and each process through its `WebView::WebContentClient`. `WebView::WebWorkerClient` does the same for a **WebWorker** process.

`WebView::CanonicalTraversable`, `WebView::CanonicalNavigable` and `WebView::CanonicalDocument` are the record that **Browser** keeps of a tab's navigables and documents, whichever processes host them.

In the **WebContent** process, a `WebContent::ConnectionFromClient` speaks to the `WebView::WebContentClient`. It has a `WebContent::PageHost`, which holds a `WebContent::PageClient` for each page, and each of those owns a `Web::Page`.

Inside **LibWeb**, a `Web::Page` has a top-level traversable, which is a navigable. Each `<iframe>` and similar element has a navigable of its own, and each navigable has an active `Web::DOM::Document`, which is the root node of the DOM tree. A navigable whose document another process hosts is a `Web::HTML::RemoteNavigable` here.

Describing the **LibWeb** object model in detail is outside the scope of this document. See [Browsing Contexts and Navigables](BrowsingContextsAndNavigables.md) for more about navigables.
