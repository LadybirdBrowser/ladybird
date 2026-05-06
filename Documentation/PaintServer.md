# PaintServer

Behavior described here can be transitional, confusing, or due for cleanup.

## How painting is different

LibWeb still walks the paint tree and records drawing. The recorded form is closer to a transport payload for PaintServer than the older cached display list format.

Resources are discovered in the course of drawing, registration/upload happens through SharedImage IPC, registries, and ResourceTransfer payloads over the arena.

The arena are shared memory rings to reduce allocation churn. They provide a communication channel between WebContent and PaintServer for draw commands, registrations, and tiny inline data.

The AccumulatedVisualContext tree is bundled into a payload footer by DisplayListRecorder/Submitter, and its structure remains pretty much intact.

This branch treats a nested display list as a more structural boundary with its own metadata, namespace, scroll map, external content references. It is not yet a fully isolated scene subtree, but we move in that direction.

The important pieces are:

- LibGfx: Resource/\*.\*, BitmapInfo.\*, MetalImage.\*, VulkanImage.\*, SharedImage\*.\*
- LibWeb/Painting: DisplayListRecorder.\*, DisplayListSubmitter.\*, EtxernalContentSource.\*, OffscreenPaintRequest.\*
- LibPaintServer: \*\*/\*Arena\*.\*, Compositor/DrawCommands.\*, RenderClientOfPaintServer.\*
- Services/PaintServer: Compositor/\*.\*, RenderClient/RenderClientConnection.\*, Painter.\*, Server.\*

## Resource registration

There are two major resource families:

- bitmaps and decoded image frames
- Local fonts and typefaces

The resource-registration path is intentionally separate from the content-image path.

### bitmap registration

The WebContent (source-side) bitmap path looks like this:

1. Paint recording encounters an image-like thing.
2. DisplayListRecorder asks its PaintCommandEncodingContext to `ensure_bitmap_resource(...)`.
3. DocumentFramePainter has already chosen whether that means:
   - a real PaintServer-backed page submission context, or
   - a local fallback context for when no sink exists.
4. we eventually emit ResourceTransfer payloads on the arena.
5. PaintServer receives the registration and imports the SharedImagePayload into its own ResourceManager.

### font registration

Fonts are similar but not identical:

1. paint recording asks `ensure_font_resource(...)`.
2. FontResourceRegistry maps a font or typeface identity to a ResourceID.
3. Pending font payloads (either a serialized SkFontface or a local font identifier) are collected and sent with the frame.
4. PaintServer loads any local fonts from disk, materializes the SkFontFaces, and caches them in FontResourceCache per surface.

## How many registries are there, and where do they live?

The answer depends on whether you count only long-lived registries or also the local fallback ones.

- Gfx::BitmapRegistry: Lives in WebContent at LibGfx/Resource/BitmapResource.\*
  - Maintains the source-side mapping from ResourceID to shared-image-backed bitmap payload.
  - Tracks descriptions and pending transfers.
- Gfx::FontResourceRegistry: Lives in Webcontent at LibGfx/Resource/FontResource.\*
  - Maintains the source-side mapping from font identity to ResourceID.
  - Queues typeface and local-font payloads for submission.
- PaintServer::ResourceManager: Lives in PaintServer, with an instance for each RenderClientConnection
  - it's the authoritative store for imported bitmap resources and content images.
  - manages arena mappings for shared blobs.
  - It owns the server-side images table (which can cause some ImageID confusion).
- PaintServer::FontResourceCache: Lives in PaintServer, with an instance per Surface.
  - Maps WebContent ResourceIDs to actual Skia typeface objects.

Oh yes and DocumentFramePainter has a LocalPaintEncodingState which in turn has a BitmapRegistry and a FontResourceRegistry. This just allows recording to function when there is no PaintServer sink. It's scheduled for deletion and probalby safe to ignore.

## The ID situation

ResourceID is client allocated and identifies a reusable registered handle for things (bitmaps, fonts) submitted as resources.

ImageID is (usually) chosen by the client too, monotonically. But depending on the path, it can mean content image, imported external image, canvas target, presentation-related image handle, offscreen result, or just “the number used to look up an entry in the server’s image table”.

When you see draw-command image references carrying both, there are generally two cases:

1. Registered bitmap resource: image_resource_id is meaningful, and image_id is a numeric alias.
2. Content image / canvas: image_id is the real thing and image_resource_id may be zero or irrelevant.

## External content and upload path:

1. Client picks a fresh ImageID.
2. Client asks PaintServer to `create_content_image(...)`.
3. PaintServer allocates the backing and returns a SharedImagePayload.
4. Clinet either uploads into that payload locally or wraps it in a PaintingSurface and renders into it.
5. Client reports `complete_content_upload(...)`.

If WebContent already owns a suitable SharedImagePayload, it can skip allocation-plus-upload and instead call `import_content_image(...)`. That path matters for producers that already have transportable backing.

ExternalContentSource now tracks a content ImageID, a SharedImagePayload, allocation/upload state, any reusable PaintingSurface, and any observers waiting for finalization.

## SVG differences

SVG image availability is not at all synchronous.

- SVGDecodedImageData uses a type of non-presentation frame render called OffscreenPaintRequest to rasterize images. So WebContent asks PaintServer to render an SVG offscreen and receives an image id it can cache along with size.

Masks, patterns, and clip-related detached display lists now record using the active recorder/tree. This is one of the less obvious behavioral differences.

## OffscreenPaintRequest

OffscreenPaintRequest is the common abstraction for every render that does not target presentation. It's a distinct payload type on the arena used for screenshots, svg decoded images, things like that. It's the entrypoint for screenshot readback. A PageClient can use it to request any kind of offscreen document rendering. They can request it submit immediately (svg) or wait for unresolved external content to finish.

Important distinction: Steady-state Canvas2D remote rendering does not use OffscreenPaintRequest. It has its own direct draw-list path.

## Canvas2D remote render flow

Canvas2D uses PaintServer::CanvasPainter, which records a PaintServer DrawList directly. And currently, it's kind of glued into ExternalContentSource as well.

When `HTMLCanvasElement::present()` runs for a 2D canvas:

1. Ask the context whether it has recorded draw commands.
2. Ask ExternalContentSource for a canvas render target via `ensure_canvas_render_target(...)`.
3. If no ready target exists yet, wait for finalization and retry.
4. Take the recorded canvas draw list.
5. Submit it with `DisplayListSubmitter::submit_canvas_draw_list(...)`.
6. PaintServer renders that draw list into the target content image.
7. Completion is reported back through `ExternalContentSource::did_complete_canvas_render(...)`.

So Canvas2D drawing is remote replay into a persistent content image, not local raster plus upload. Readback tries to read from the PaintServer-backed content surface when possible. That's what `paint_server_surface_for_2d_context_readback()` is about.
