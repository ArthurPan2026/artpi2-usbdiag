# Board-side PDF font assets

## Simplified Chinese font subset

- Upstream: `notofonts/noto-cjk`, tag `Sans2.004`, file `Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf`.
- Upstream SHA-256: `D68BAFCB48A2707749396AA12BBBD833CB70401F3A9A689FD2902C7E0D295964`.
- Derived file: `NotoSansSC-ARTPi2-subset.ttf`, instantiated at weight 400 and subset to the GB2312 character set plus Unicode characters used by the Web report.
- Derived SHA-256: `22B3E753D64C7526E3CC40BD4863CA3CAD11A8DF6AA39BA42C0A28CE844BB8BD`.
- FontTools version used for the checked-in output: 4.67.0. Firmware builds consume this checked-in subset and do not install or run FontTools.
- License: SIL Open Font License 1.1; see `OFL-NotoSansSC.txt`.
- `../pdf_font_coverage.js` records the BMP glyph coverage for the CMake font-asset generator. The generator embeds its compact bitmap with the font for the board-side libHaru renderer. Unsupported dynamic characters are written as visible `[U+XXXX]` markers in the PDF.

## Runtime use

The board-side libHaru renderer embeds this font into its PDF output. The Web client downloads and verifies that completed PDF; it does not construct reports with a browser JavaScript PDF library.
