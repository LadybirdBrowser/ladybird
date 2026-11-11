# Ladybird process architecture

Ladybird uses a multi-process architecture to improve stability and security in the face of arbitrary (and possibly hostile) web content. Web content runs in sandboxed processes, and work that handles untrusted data, such as parsing images, is split off into further sandboxed helpers. A helper that crashes takes down as little as possible, and a compromised one reaches as little as possible.

## Process overview

```
                             UI
                              |
                             Hub
                              |
     +----------+-------------+-------+----------------+
     |          |                     |                |
 Compositor  WebContent         RequestServer    ImageDecoder
             (renderer)         (per site)       (per site)
                 |
     +-----------+-----------+
     |           |           |
 WebRTCClient  MediaServer  AudioServer
                 |
         VideoDecoderServer
```

**RequestServer** and **ImageDecoder** are shared by the renderers of a site, while the processes below the renderers each work for a single renderer. **Hub** launches all of them, because the sandboxes keep the others from spawning anything. It then connects them directly to each other, so that a renderer talks to its **RequestServer** or **ImageDecoder** without going through **Hub**.

| Process | How many | Launched |
| --- | --- | --- |
| **UI** | One | By the user |
| **Hub** | One | By **UI**, as it starts |
| **Compositor** | One | At startup, and again if it crashes |
| **WebContent** | One per set of documents that must share a process, see [site isolation](#site-isolation) | When a document needs one, with a spare kept ready for the next tab |
| **RequestServer** | One per browsing session and top-level site, plus one per session with no site | The default session's one with no site at startup, the others on first use |
| **ImageDecoder** | One per browsing session and site, or one for a single process | When a renderer first has an image to decode |
| **MediaServer** | One per renderer | When the renderer first needs one |
| **AudioServer** | One per renderer | When the renderer first needs one |
| **VideoDecoderServer** | One per MediaServer | When its **MediaServer** asks for one |
| **WebRTCClient** | One per **WebContent** process | When the renderer first needs one |

On macOS, the kernel does not end a process when its parent dies, so a small **ProcessReaper** helper ends the others when **Hub** is gone.

### Process: UI

This is the application itself, and the only process that talks to the window system. It draws windows, tabs and menus, shows the frames that the **Compositor** presents, and turns what the user does into input for **Hub**. It shows pages as rendered images and receives some of their text, but it never hosts a page's DOM or runs its JavaScript. It is not sandboxed yet.

**UI** and **Hub** talk over the protocols in `Services/Hub/HubServer.ipc` and `Services/Hub/HubClient.ipc`. **UI** may wait for **Hub** to answer, but **Hub** never waits for **UI**, which is why they cannot deadlock. Neither outlives the other: **Hub** ends once the connection closes, and **UI** ends with it if **Hub** is gone.

With `--headless`, **UI** has nothing to do, and **Hub** takes over its process. On Windows, **Hub** does not have a process of its own yet, and runs inside **UI** over the same protocols.

### Process: Hub

This process launches and connects all the others, and it is not sandboxed yet. It owns the state that must outlive or be shared between renderers: cookies, web storage, blob URLs, permissions and the fonts of the system. Other processes ask **Hub** for these over IPC. Browsing history, bookmarks and favicons belong to **UI**.

**Hub** also keeps its own record of every tab's navigables and documents, and decides which process hosts each document.

### Process: Compositor

This process turns what renderers record into pixels. Renderers send it display lists, which it rasterizes and presents, along with their canvases, WebGL contexts and video frames. It does the GPU work of rendering, so renderers need no access to the GPU.

It also runs animated images and scrolling, which keeps them moving while a renderer's main thread is busy. For animated images, it gets a connection of its own to the renderer's **ImageDecoder**.

If the **Compositor** crashes, **Hub** launches a new one and reconnects the renderers to it.

### Process: WebContent

This process hosts the main HTML/CSS engine (**LibWeb**) and runs JavaScript (**LibJS**). It gets input events from **Hub** and records its pages for the **Compositor**. It reaches the network only through **RequestServer**, and local files only through **Hub**.

A process holds one page for each tab it has documents in.

Web workers run in this process too, each on a thread of its own with its own JavaScript VM and heap. A dedicated worker runs in the process of its owner. A shared worker runs in the process of the document that starts it, and that process stays alive without pages for as long as it runs a shared worker, as the worker's other owners can be in other processes. A shared worker that may not share a process with its owner, as the owner is cross-origin isolated, gets a **WebContent** process without a page.

### Process: RequestServer

This process uses networking protocols (like HTTP or HTTPS) to make requests on behalf of renderers, and resolves DNS names itself. It asks **Hub** for the cookies of each request, and hands **Hub** the cookies it receives.

Each top-level site has a **RequestServer** of its own in each browsing session, which only makes and caches requests for that site's partitions. Its sandbox confines its cache to that site's directory, and the process rejects requests that a renderer is not bound to make. A session's **RequestServer** with no site serves **Hub** itself and requests made under no site. A site's **RequestServer** ends once it has been idle for a while.

### Process: ImageDecoder

This process decodes images (PNG, JPEG, BMP, ICO, PBM, etc.) into bitmaps. It is strongly sandboxed, and can do little more than receive encoded image data and return bitmaps to its clients. It decodes into shared memory, which it hands to its clients as it is.

A decoder sees the images of every process it serves, so a malicious image that takes one over gets at those images. With site isolation, the processes of a site share an **ImageDecoder**, because they hold nothing of any other site's that they could not fetch for themselves. The site is that of the documents a process hosts, not their top-level site, so a frame never shares a decoder with the site embedding it, unless it has an opaque origin, which keeps it in the process of the document that made it. Private browsing is kept apart. Any other process, such as one with no site, gets a decoder of its own, and a process that comes to host a second site leaves the decoder of the first.

**Hub** launches a decoder when a process first has an image to decode, and ends it once each process it serves is gone or has let go of it. A process lets go of its decoder once it has gone 30 seconds with nothing to decode and no animation playing. Like a process that loses its decoder to a crash, it asks for a new one when it next has an image to decode.

### Processes: MediaServer, AudioServer and VideoDecoderServer

**MediaServer** plays the media of a renderer. Its video decoders run in a **VideoDecoderServer**, which **Hub** launches on its request. **AudioServer** talks to the audio devices of the system, for both a renderer's **MediaServer** and its WebAudio output.

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

`WebView::Process` spawns every helper. It creates the IPC transport first, gives the helper its end, and passes it only the environment variables that the helper needs. On Linux, the transport is a socket pair, and the helper finds its end through the `SOCKET_TAKEOVER` environment variable. On macOS, it is a pair of Mach ports, which the helper fetches from **Hub** by name. On Windows, it is a pipe, along with a connection to a handle broker in **Hub**, since a pipe cannot carry handles.

Once two processes need to talk, **Hub** creates a channel and hands an end to each of them as an `IPC::TransportHandle`. Helpers that serve several clients, such as **RequestServer** and **ImageDecoder**, have a separate control connection that only **Hub** speaks, which it uses to connect new clients.

A renderer that needs a helper it does not have yet asks **Hub** for a connection to it over IPC.

## Sandboxing

Every process in the table above except **UI** and **Hub** applies a sandbox to itself early in its startup, unless **Hub** was started with `--disable-sandbox`. Each one's sandbox allows only what that process needs, and can be found in the `Sandbox` files in its directory under `Services`. The one of **WebContent** is in `Services/RendererSandbox.h`.

On Linux, sandboxes are built from seccomp and Landlock. On kernels without Landlock, seccomp traps every system call that takes a path, and a broker process forked from the helper carries out the ones that the helper's path list permits. On macOS, they use the Seatbelt sandbox. Windows has no sandbox yet.

A system call that a Linux sandbox kills the process for is always logged. Most refusals only make the call fail, though, and the process may never say so. To see those too, start Ladybird with `LADYBIRD_SANDBOX_DEBUG=1`. Every helper then logs each system call that its seccomp policy refuses without killing it, and confines its files with the broker even where Landlock exists, so that it can log each path that it refuses. The broker decides by path where Landlock decides by file, and it does not scope signals or abstract sockets, so a refusal that only Landlock causes does not happen in this mode.

## Class overview

```
UI process

    Ladybird::WebContentView           The widget showing a tab
      Ladybird::RemoteView             What the UI knows of the tab's view, and how it reaches it

  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

Hub process

    WebView::ViewImplementation        The view of a tab, such as Hub::View
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

In the **Hub** process, each kind of view derives from `WebView::ViewImplementation`, which shows a tab. A tab's documents may be spread over several processes, so a view reaches each of them through a `WebView::WebContentPage`, and each process through its `WebView::WebContentClient`. `WebView::WebWorkerClient` does the same for a worker on a thread of a **WebContent** process.

`WebView::CanonicalTraversable`, `WebView::CanonicalNavigable` and `WebView::CanonicalDocument` are the record that **Hub** keeps of a tab's navigables and documents, whichever processes host them.

In the **WebContent** process, a `WebContent::ConnectionFromClient` speaks to the `WebView::WebContentClient`. It has a `WebContent::PageHost`, which holds a `WebContent::PageClient` for each page, and each of those owns a `Web::Page`.

Inside **LibWeb**, a `Web::Page` has a top-level traversable, which is a navigable. Each `<iframe>` and similar element has a navigable of its own, and each navigable has an active `Web::DOM::Document`, which is the root node of the DOM tree. A navigable whose document another process hosts is a `Web::HTML::RemoteNavigable` here.

Describing the **LibWeb** object model in detail is outside the scope of this document. See [Browsing Contexts and Navigables](BrowsingContextsAndNavigables.md) for more about navigables.
