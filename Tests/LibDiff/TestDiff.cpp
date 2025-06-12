/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_generate_unified_diff)
{
    StringView old_text = R"(Viewport <#document> at (0,0) content-size 800x600 children: not-inline
  BlockContainer <html> at (0,0) content-size 800x600 [BFC] children: not-inline
    BlockContainer <body> at (8,8) content-size 784x150 children: not-inline
      BlockContainer <(anonymous)> at (8,8) content-size 784x0 children: inline
        TextNode <#text>
        TextNode <#text>
      BlockContainer <div> at (8,8) content-size 784x150 children: inline
        line 0 width: 300, height: 150, bottom: 150, baseline: 150
          frag 0 from SVGSVGBox start: 0, length: 0, rect: [8,8 300x150]
        TextNode <#text>
        SVGSVGBox <svg> at (8,8) content-size 300x150 [SVG] children: inline
          TextNode <#text>
          Box <use> at (8,8) content-size 0x0 children: inline
            Box <symbol#braces> at (8,8) content-size 0x0 children: inline
              TextNode <#text>
              SVGGeometryBox <path> at (92.375,26.75) content-size 131.25x112.15625 children: inline
                TextNode <#text>
              TextNode <#text>
          TextNode <#text>
        TextNode <#text>

ViewportPaintable (Viewport<#document>) [0,0 800x600]
  PaintableWithLines (BlockContainer<HTML>) [0,0 800x600]
    PaintableWithLines (BlockContainer<BODY>) [8,8 784x150]
      PaintableWithLines (BlockContainer(anonymous)) [8,8 784x0]
      PaintableWithLines (BlockContainer<DIV>) [8,8 784x150]
        SVGSVGPaintable (SVGSVGBox<svg>) [8,8 300x150]
          PaintableBox (Box<use>) [8,8 0x0]
            PaintableBox (Box<symbol>#braces) [8,8 0x0]
              SVGGeometryPaintable (SVGGeometryBox<path>) [92.375,26.75 131.25x112.15625]

)"sv;

    StringView new_text = R"(Viewport <#document> at (0,0) content-size 800x600 children: not-inline
  BlockContainer <html> at (0,0) content-size 800x600 [BFC] children: not-inline
    BlockContainer <body> at (8,8) content-size 784x150 children: not-inline
      BlockContainer <(anonymous)> at (8,8) content-size 784x0 children: inline
        TextNode <#text>
        TextNode <#text>
      BlockContainer <div> at (8,8) content-size 784x150 children: inline
        line 0 width: 300, height: 150, bottom: 150, baseline: 150
          frag 0 from SVGSVGBox start: 0, length: 0, rect: [8,8 300x150]
        TextNode <#text>
        SVGSVGBox <svg> at (8,8) content-size 300x150 [SVG] children: inline
          TextNode <#text>
          Box <use> at (8,8) content-size 0x0 children: not-inline
          TextNode <#text>
        TextNode <#text>

ViewportPaintable (Viewport<#document>) [0,0 800x600]
  PaintableWithLines (BlockContainer<HTML>) [0,0 800x600]
    PaintableWithLines (BlockContainer<BODY>) [8,8 784x150]
      PaintableWithLines (BlockContainer(anonymous)) [8,8 784x0]
      PaintableWithLines (BlockContainer<DIV>) [8,8 784x150]
        SVGSVGPaintable (SVGSVGBox<svg>) [8,8 300x150]
          PaintableBox (Box<use>) [8,8 0x0]

)"sv;

    auto result = MUST(Diff::from_text(old_text, new_text, 3));
    EXPECT_EQ(result.size(), 2U);

    auto hunk1_stream = make<AllocatingMemoryStream>();
    MUST(Diff::write_unified(result[0], *hunk1_stream));

    auto hunk1 = MUST(hunk1_stream->read_until_eof());
    EXPECT_EQ(StringView { hunk1 }, R"(@@ -10,12 +10,7 @@
         TextNode <#text>
         SVGSVGBox <svg> at (8,8) content-size 300x150 [SVG] children: inline
           TextNode <#text>
-          Box <use> at (8,8) content-size 0x0 children: inline
-            Box <symbol#braces> at (8,8) content-size 0x0 children: inline
-              TextNode <#text>
-              SVGGeometryBox <path> at (92.375,26.75) content-size 131.25x112.15625 children: inline
-                TextNode <#text>
-              TextNode <#text>
+          Box <use> at (8,8) content-size 0x0 children: not-inline
           TextNode <#text>
         TextNode <#text>
 
)"sv);

    auto hunk2_stream = make<AllocatingMemoryStream>();
    MUST(Diff::write_unified(result[1], *hunk2_stream));

    auto hunk2 = MUST(hunk2_stream->read_until_eof());
    EXPECT_EQ(StringView { hunk2 }, R"(@@ -26,6 +21,4 @@
       PaintableWithLines (BlockContainer<DIV>) [8,8 784x150]
         SVGSVGPaintable (SVGSVGBox<svg>) [8,8 300x150]
           PaintableBox (Box<use>) [8,8 0x0]
-            PaintableBox (Box<symbol>#braces) [8,8 0x0]
-              SVGGeometryPaintable (SVGGeometryBox<path>) [92.375,26.75 131.25x112.15625]
 
)"sv);
}
