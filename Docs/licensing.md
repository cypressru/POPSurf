# Licensing

Original POPSurf code is licensed under [0BSD](../LICENSE). This permits use,
copying, modification, and distribution without an attribution requirement.

Bundled code and assets retain their upstream licenses:

| Component | Location | License |
| --- | --- | --- |
| litehtml | `vendor/litehtml/` | BSD 3-Clause |
| Gumbo | `vendor/litehtml/src/gumbo/` | Apache 2.0 |
| stb_image | `vendor/stb_image.h` | MIT or public domain |
| stb_truetype | `vendor/stb_truetype.h` | MIT or public domain |
| Noto Sans Regular 2.015 | `cd/font.ttf` | SIL Open Font License 1.1 |
| TimGM6mb-derived MIDI bank | `cd/gmbank.psb` | GNU GPL 2.0 |
| Ruffle-derived SWF routines | `swf/` | MIT |

Third-party notices must remain with redistributed source and release images.

Several SWF parsing and geometry routines were derived from Ruffle. The
applicable MIT notice is in `LICENSES/Ruffle-MIT.txt`.

`cd/font.ttf` is the hinted Noto Sans Regular TTF from the official
[NotoSans 2.015 release](https://github.com/notofonts/latin-greek-cyrillic/releases/tag/NotoSans-v2.015).
Its SHA-256 is
`478c558ec25cc5ebd44b2b9b40530412f790cc8b5041393224c9f0a7e9f01e22`.
The required notice and license are in `LICENSES/Noto-Sans-OFL-1.1.txt`.

`cd/gmbank.psb` was baked from TimGM6mb 1.3 with `tools/gmbake.py`. The source
SoundFont has SHA-256
`c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854`.
TimGM6mb is distributed separately under GPL 2.0; it is not covered by
POPSurf's 0BSD license. Its license is in
`LICENSES/TimGM6mb-GPL-2.0.txt`, and the original SoundFont is available from
[Tim Brechbill's distribution page](https://timbrechbill.com/saxguru/Timidity.php).
