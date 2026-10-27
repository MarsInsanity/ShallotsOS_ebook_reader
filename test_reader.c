/* Run: gcc -I../mupdf/include -ffunction-sections -fdata-sections test_reader.c
 * -Wl,--gc-sections -o /tmp/test-ebook-reader && /tmp/test-ebook-reader */
#define main reader_main
#include "reader.c"
#undef main
#include <assert.h>

int main(void) {
    char dir[] = "/tmp/ebook-check-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    FILE *f = fopen("book.epub.pos", "w");
    assert(f && fprintf(f, "7 26 1 2\n") > 0);
    assert(fclose(f) == 0);
    font_count = 3;
    pos_init("book.epub");
    pos_load();
    assert(page == 7 && font_px == 26 && font_sel == 1 && theme == TH_DARK);
    assert(access("book.epub.pos", F_OK) != 0);
    page = 9;
    pos_mark_dirty();
    pos_save();
    assert(!pos_dirty);
    page = 0;
    pos_load();
    assert(page == 9 && theme == TH_DARK);
    strcpy(pos_path, "missing/book.pos");
    pos_mark_dirty();
    pos_save();
    assert(pos_dirty); /* Failed saves must remain pending. */

    PW = 4;
    PH = FOOTER_H + 2;
    FB = calloc((size_t)PW * PH, sizeof(*FB));
    assert(FB);
    unsigned char samples[] = {0, 0, 0, 0, 0, 0};
    fz_pixmap pix = {0};
    pix.w = 2; pix.h = 1; pix.n = 3; pix.stride = 6; pix.samples = samples;
    theme = TH_LIGHT;
    pix_to_fb(&pix);
    assert(FB[0] == 0xffff && FB[1] == 0 && FB[2] == 0 && FB[3] == 0xffff);
    assert(FB[PW * reading_height()] == 0xffff);
    free(FB);

    /* ShallotsOS position lines add the chapter and the fraction of it. */
    strcpy(pos_path, ".positions/book.epub.pos");
    f = fopen(pos_path, "w");
    assert(f && fprintf(f, "12 30 0 1 3 0.250000\n") > 0);
    assert(fclose(f) == 0);
    pos_has_spot = 0;
    pos_load();
    assert(page == 12 && font_px == 30 && theme == TH_SEPIA);
    assert(pos_has_spot && pos_spot.ch == 3 && pos_spot.frac > 0.24f && pos_spot.frac < 0.26f);

    /* Bookmarks round-trip through the .marks file beside the position. */
    snprintf(marks_path, sizeof marks_path, ".positions/book.epub.marks");
    nmarks = 2;
    marks[0] = (Spot){ 1, 0.5f };
    marks[1] = (Spot){ 4, 0.0f };
    marks_save();
    nmarks = 0;
    marks_load();
    assert(nmarks == 2 && marks[0].ch == 1 && marks[1].ch == 4);
    assert(marks[0].frac > 0.49f && marks[0].frac < 0.51f);
    nmarks = 0;
    marks_save();
    assert(access(marks_path, F_OK) != 0);   /* no bookmarks: no file */

    /* Chapter navigation over the contents. */
    count = 100;
    add_chapter("  One\n", 0, 0);
    add_chapter("Two", 0, 30);
    add_chapter("Two, part b", 1, 45);
    add_chapter("Three", 0, 70);
    add_chapter("Out of range", 0, 120);
    assert(nchapters == 4 && !strcmp(chapters[0].title, "One "));
    assert(chapter_at(0) == 0 && chapter_at(29) == 0 && chapter_at(30) == 1);
    assert(chapter_at(50) == 2 && chapter_at(99) == 3);
    assert(next_chapter_page(10) == 30 && next_chapter_page(70) == -1);
    assert(prev_chapter_page(35) == 30 && prev_chapter_page(30) == 0);
    assert(prev_chapter_page(0) == -1);
    puts("Reader save/migration, bookmark, chapter and page placement checks passed.");
    return 0;
}
