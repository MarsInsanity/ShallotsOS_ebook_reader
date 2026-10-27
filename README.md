# ShallotsOS Ebook Reader

The Books reader of ShallotsOS, based on the TreeFrogUI ebook reader: a
small standalone ebook/document reader for the MIPS Hichip handhelds
(R36SX / SF3000 / SF3000 HD / SF3100 / SF3500 / GB350).
Reads **EPUB, MOBI, PDF, CBZ, FB2, XPS** - everything MuPDF handles.

It is launched by the menu as a standalone (like `pcsx4all`): the Books
folder maps to the `ebook` binary, which is run with the book path as its
argument.

## What it is (and isn't)

- **`reader.c`** - first-party glue (~350 lines): MuPDF renders a page ->
  converted to RGB565 -> presented via `driver.so` (the shared `hwdisp` path
  used by the other standalones) -> input from cubevol's `/tmp/joy_key` shm.
- **MuPDF** - the rendering engine, used **unmodified** as an upstream library
  (vendored + cross-compiled, no source patches). Not forked.
- **`port_sf3000/`** - the display harness (`hwdisp.c`, `tf_driver.h`) copied
  from the pcsx4all port so all standalones present the same way on every device.

## Controls

| Button | Reading |
|--------|---------|
| R1 / RIGHT / A | next page (hold to keep turning) |
| L1 / LEFT / B | previous page |
| DOWN / UP | next chapter / start of this or the previous chapter |
| R2 / L2 | jump +/- 10% |
| Y | add or remove a bookmark on this page |
| X | contents |
| SELECT | menu |
| START | show / hide the button hints (shown for a few seconds when a book opens) |
| SELECT + START | quit (progress saved) |

The menu has Contents, Bookmarks, a bookmark toggle, Text size, Font, Theme
(Light / Sepia / Dark), Go to start / end and Quit. UP/DOWN move, A selects,
LEFT/RIGHT change a value (applied when the menu closes), B or SELECT
closes. In Bookmarks, X deletes the selected one; L1/R1 page through long
lists.

Menus and lists use the colours of the current ShallotsOS theme
(`cubegm/skin/skin.txt`). The footer shows the chapter, page and percent.

On 4:3 panels (R36SX, GB350) pages are rendered at 640x480 and shown 1:1;
on the 16:9 devices at 1280x720, which the driver scales to the panel.

Reading position, text size, font face and theme are saved per book under a
hidden `.positions/` subdirectory beside the books, with the bookmarks in a
`.marks` file next to it. Positions are stored as a chapter and a fraction of
it, so they stay put when the text size or font changes. Older `<book>.pos`
sidecars are migrated when the book is opened.

### Fonts

Built-in Serif / Sans / Mono, plus any `.ttf` / `.otf` dropped in
`frogui/fonts/`, `cubegm/fonts/`, or `roms/Ebook/fonts/`, `Books/fonts/` (loaded via `@font-face`).

## Build

Needs the SF3000 MIPS toolchain (see the main TreeFrogUI workspace).

```sh
# 1. Vendor + cross-build MuPDF (unmodified upstream, pinned tag).
git clone --depth 1 --branch 1.24.10 --recurse-submodules --shallow-submodules \
    https://github.com/ArtifexSoftware/mupdf.git ../mupdf
./build_mupdf_sf.sh        # -> ../mupdf/build/release/libmupdf*.a

# 2. Build the reader (links libmupdf statically; libstdc++ static-linked too).
./build_reader_sf.sh       # -> ./ebook  (MIPS binary, ~38 MB with fonts)

# Both scripts take MUPDF=... and TOOLCHAIN=... from the environment
# (defaults: ~/sf3000-work/mupdf and ~/sf3000-work/sf3000toolchain/...).
# ShallotsOS's overlay workflow builds both in CI.
```

Copy `ebook` to `cubegm/ebook` on the SD card.

## License

The reader glue is provided under the **GNU AGPL-3.0** to match MuPDF, which it
links statically. **MuPDF is AGPL-3.0** ([Artifex](https://mupdf.com/licensing));
distributing this binary makes the whole thing AGPL. If you need a
non-AGPL/commercial distribution, obtain a commercial MuPDF license from Artifex.
