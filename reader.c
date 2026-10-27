/*
 * ShallotsOS ebook reader (standalone) - MuPDF backend.
 * Based on the TreeFrogUI ebook reader.
 *
 * Renders epub / mobi / pdf / cbz / fb2 / xps to RGB565 and blits through
 * driver.so (hwdisp), same present path as lgpt/rockbox/pcsx4all. Input via
 * cubevol's /tmp/joy_key shared memory.
 *
 * Reading:
 *   R1 / RIGHT / A   -> next page        L1 / LEFT / B -> previous page
 *   DOWN / UP        -> next / previous chapter
 *   R2 / L2          -> jump +/-10%
 *   Y                -> add / remove a bookmark on this page
 *   X                -> contents (chapter list)
 *   SELECT           -> menu;  SELECT+START anytime -> quit (progress saved)
 * Menu and lists: UP/DOWN move, A select, B/SELECT close, LEFT/RIGHT change
 * a value; in Bookmarks, X deletes the selected one.
 *
 * Progress and bookmarks are saved under a hidden ".positions" directory
 * beside the book, as a chapter and a fraction of it, so they survive a
 * change of text size or font.
 */
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#include "port_sf3000/hwdisp.h"

/* joy_key bits (cubevol; see picoarch plat_sdl.c). */
enum { K_SELECT=0, K_START=3, K_UP=4, K_RIGHT=5, K_DOWN=6, K_LEFT=7,
       K_L2=8, K_R2=9, K_L1=10, K_R1=11, K_X=12, K_A=13, K_B=14, K_Y=15 };
#define BIT(b) (1u<<(b))

static volatile uint32_t *g_keys;
static int   PW = 1280, PH = 720;      /* canvas pixels */
static float S = 1.5f;                 /* UI scale: canvas height / 480 */
static uint16_t *FB;                    /* PW*PH RGB565 */

#define FOOTER_H ((int)(30*S + 0.5f))
static int reading_height(void) {
    int h = PH - FOOTER_H;
    return h > 0 ? h : PH;
}

static void input_init(void) {
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) return;
    int id = shmget(k, 4, IPC_CREAT | 0666);
    if (id < 0) return;
    void *p = shmat(id, NULL, 0);
    if (p != (void *)-1) g_keys = (uint32_t *)p;
}
static uint32_t keys(void) { return g_keys ? (*g_keys & 0xFFFF) : 0; }

static uint64_t ticks_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Key presses with auto-repeat for held direction/page keys. */
#define REPEAT_KEYS (BIT(K_UP)|BIT(K_DOWN)|BIT(K_LEFT)|BIT(K_RIGHT)|BIT(K_L1)|BIT(K_R1))
static uint32_t pressed(uint32_t *prev, uint64_t *held_since, uint64_t *last_rep) {
    uint32_t k = keys(), pr = k & ~*prev;
    uint64_t now = ticks_ms();
    if (pr) { *held_since = now; *last_rep = now; }
    else if ((k & REPEAT_KEYS) && (k & REPEAT_KEYS) == (*prev & REPEAT_KEYS) &&
             now - *held_since >= 400 && now - *last_rep >= 110) {
        pr = k & REPEAT_KEYS;
        *last_rep = now;
    }
    *prev = k;
    return pr;
}

/* ---- the canvas: 640x480 on 4:3 panels, 1280x720 elsewhere ----------------
 * The driver scales whatever it is given to the panel. 1280x720 on the 4:3
 * R36SX panel was squeezed more across than down, so pages came out
 * stretched vertically; there a 640x480 canvas goes on screen 1:1 (sharper
 * too). 1280x720 stays the size for the 16:9 devices, where it is proven. */
static void canvas_init(void) {
    int w = 0, h = 0, four3 = 0;
    FILE *f = fopen("/tmp/tfdevice.env", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "TF_DEVICE=", 10))
                four3 |= !strcasecmp(line + 10, "R36SX") || !strcasecmp(line + 10, "GB350");
            else if (!strncmp(line, "TF_PANEL_W=", 11)) w = atoi(line + 11);
            else if (!strncmp(line, "TF_PANEL_H=", 11)) h = atoi(line + 11);
        }
        fclose(f);
    }
    if (w > 0 && h > 0) four3 = w * 3 == h * 4;
    if (four3) { PW = 640; PH = 480; }
    else       { PW = 1280; PH = 720; }
    S = PH / 480.0f;
}

/* ---- colours: the ShallotsOS theme's, for menus and lists ------------------ */
static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
}
static uint16_t hex565(uint32_t c) { return rgb565((c>>16)&255, (c>>8)&255, c&255); }
static uint16_t mix565(uint16_t a, uint16_t b, int t /* 0..255 of b */) {
    int r = (((a>>11)&31)*(255-t) + ((b>>11)&31)*t) / 255;
    int g = (((a>>5)&63)*(255-t) + ((b>>5)&63)*t) / 255;
    int bl = ((a&31)*(255-t) + (b&31)*t) / 255;
    return (uint16_t)((r<<11)|(g<<5)|bl);
}
static uint16_t c_panel = 0x18C3, c_text = 0xFFFF, c_dim = 0x9CD3, c_sel = 0xB35C, c_sel_text = 0xFFFF;

static int skin_value(const char *key, uint32_t *out) {
    FILE *f = fopen("/mnt/sdcard/cubegm/skin/skin.txt", "r");
    if (!f) return 0;
    char line[256];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            const char *v = line + kl + 1;
            if (*v == '#') v++;
            *out = (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFF;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}
static int luma565(uint16_t c) {
    return ((((c>>11)&31)*255/31)*77 + (((c>>5)&63)*255/63)*150 + ((c&31)*255/31)*29) >> 8;
}
static void colours_init(void) {
    uint32_t v;
    if (skin_value("background_color", &v)) c_panel = hex565(v);
    if (skin_value("text_color", &v)) c_text = hex565(v);
    if (skin_value("selection_color", &v)) c_sel = hex565(v);
    c_dim = mix565(c_text, c_panel, 110);
    c_sel_text = luma565(c_sel) > 160 ? 0x0000 : 0xFFFF;
}

/* ---- drawing --------------------------------------------------------------- */
static void fill_rect(int x, int y, int w, int h, uint16_t col) {
    for (int j = 0; j < h; j++) { int py = y+j; if (py<0||py>=PH) continue;
        for (int i = 0; i < w; i++) { int px = x+i; if (px<0||px>=PW) continue; FB[py*PW+px] = col; } }
}
static void fill_round(int x, int y, int w, int h, int r, uint16_t col) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j; if (py < 0 || py >= PH) continue;
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        int in = 0;
        if (dy) { int dx = 0; while (dx < r && (r-dx)*(r-dx) + dy*dy > r*r) dx++; in = dx; }
        for (int i = in; i < w - in; i++) { int px = x+i; if (px>=0 && px<PW) FB[py*PW+px] = col; }
    }
}
static void dim_rect(int x, int y, int w, int h, int t /* 0..255 toward black */) {
    for (int j = 0; j < h; j++) { int py = y+j; if (py<0||py>=PH) continue;
        for (int i = 0; i < w; i++) { int px = x+i; if (px<0||px>=PW) continue;
            FB[py*PW+px] = mix565(FB[py*PW+px], 0x0000, t); } }
}

/* ---- MuPDF ----------------------------------------------------------------- */
static fz_context *ctx;
static fz_colorspace *rgb;
static fz_document *doc;
static int page = 0, count = 1;

/* The SF3000 is memory constrained. MuPDF's unlimited store keeps decoded
 * images and fonts from every visited EPUB page until exit, eventually pushing
 * the device into swap/thrashing. Keep the useful cache, but put a hard ceiling
 * on it so long/image-heavy books stay responsive. */
#define EBOOK_STORE_BYTES (16u << 20)

/* ---- UI text: MuPDF's built-in Helvetica, anti-aliased --------------------- */
static fz_font *ui_reg, *ui_bold;

static int text_w(fz_font *f, float size, const char *s) {
    if (!f || !*s) return 0;
    fz_matrix m = fz_identity;
    fz_var(m);
    fz_try(ctx) m = fz_measure_string(ctx, f, fz_scale(size, size), s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
    fz_catch(ctx) return 0;
    return (int)(m.e + 0.5f);
}

/* Cut s to fit maxw pixels, ending in "...". out holds the result. */
static const char *fit_text(fz_font *f, float size, const char *s, int maxw, char *out, size_t n) {
    if (maxw <= 0 || text_w(f, size, s) <= maxw) return s;
    size_t len = strlen(s);
    if (len >= n - 4) len = n - 4;
    while (len > 0) {
        do len--; while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80);
        memcpy(out, s, len);
        memcpy(out + len, "...", 4);
        if (text_w(f, size, out) <= maxw) return out;
    }
    snprintf(out, n, "...");
    return out;
}

/* Draw s with its baseline at y. Returns the width drawn. */
static int draw_text(int x, int y, fz_font *f, float size, const char *s, uint16_t col, int maxw) {
    char cut[256];
    s = fit_text(f, size, s, maxw, cut, sizeof cut);
    int w = text_w(f, size, s) + 2, asc = (int)(size * 1.05f) + 1, h = asc + (int)(size * 0.35f) + 2;
    if (w <= 2 || !f) return 0;
    fz_pixmap *pm = NULL;
    fz_device *dev = NULL;
    fz_text *t = NULL;
    fz_var(pm); fz_var(dev); fz_var(t);
    fz_try(ctx) {
        pm = fz_new_pixmap(ctx, NULL, w, h, NULL, 1);
        fz_clear_pixmap(ctx, pm);
        dev = fz_new_draw_device(ctx, fz_identity, pm);
        t = fz_new_text(ctx);
        fz_show_string(ctx, t, f, fz_make_matrix(size, 0, 0, -size, 1, (float)asc), s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
        float black = 0;
        fz_fill_text(ctx, dev, t, fz_identity, fz_device_gray(ctx), &black, 1.0f, fz_default_color_params);
        fz_close_device(ctx, dev);
        int ox = x - 1, oy = y - asc;
        for (int j = 0; j < h; j++) {
            int py = oy + j; if (py < 0 || py >= PH) continue;
            const unsigned char *a = pm->samples + (size_t)j * pm->stride;
            for (int i = 0; i < w; i++) {
                int px = ox + i; if (px < 0 || px >= PW || !a[i]) continue;
                uint16_t *d = &FB[py*PW+px];
                *d = a[i] == 255 ? col : mix565(*d, col, a[i]);
            }
        }
    } fz_always(ctx) {
        fz_drop_text(ctx, t);
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pm);
    } fz_catch(ctx) { return 0; }
    return w - 2;
}
static void draw_text_right(int xr, int y, fz_font *f, float size, const char *s, uint16_t col) {
    draw_text(xr - text_w(f, size, s), y, f, size, s, col, 0);
}
static void draw_text_center(int y, fz_font *f, float size, const char *s, uint16_t col) {
    draw_text((PW - text_w(f, size, s)) / 2, y, f, size, s, col, PW - (int)(32*S));
}

static void ui_init(void) {
    fz_try(ctx) {
        ui_reg = fz_new_base14_font(ctx, "Helvetica");
        ui_bold = fz_new_base14_font(ctx, "Helvetica-Bold");
    } fz_catch(ctx) { fz_report_error(ctx); }
}

/* ---- font size (stored as on-panel px; the render em scales with the
 * canvas, which is S times a 480-line panel) ------------------------------ */
static int  font_px = 24;                 /* what the user sets/sees */
#define PX_MIN 12
#define PX_MAX 48
static float em_from_px(void) { return font_px * S; }

/* ---- font face: built-in serif/sans/mono + any TTF/OTF dropped in a fonts
 * dir. Custom faces are injected via an @font-face user CSS pointing at the
 * absolute file path. ------------------------------------------------------ */
typedef struct { char name[48]; char css[512]; int builtin; } FontEntry;
static FontEntry fonts[32];
static int font_count = 0, font_sel = 0;

static void add_builtin(const char *name, const char *family) {
    FontEntry *e = &fonts[font_count++];
    snprintf(e->name, sizeof e->name, "%s", name);
    if (family[0]) snprintf(e->css, sizeof e->css, "* { font-family: %s !important; }", family);
    else e->css[0] = 0;
    e->builtin = 1;
}
static void scan_font_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && font_count < 32) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf"))) continue;
        FontEntry *e = &fonts[font_count++];
        snprintf(e->name, sizeof e->name, "%.*s", (int)(dot - de->d_name) < 47 ? (int)(dot - de->d_name) : 47, de->d_name);
        snprintf(e->css, sizeof e->css,
                 "@font-face{font-family:\"ebk\";src:url(\"%s/%s\")}"
                 "*{font-family:\"ebk\" !important}", dir, de->d_name);
        e->builtin = 0;
    }
    closedir(d);
}
static void fonts_init(void) {
    add_builtin("Serif", "");
    add_builtin("Sans", "sans-serif");
    add_builtin("Mono", "monospace");
    scan_font_dir("/mnt/sdcard/frogui/fonts");
    scan_font_dir("/mnt/sdcard/cubegm/fonts");
    scan_font_dir("/mnt/sdcard/roms/Ebook/fonts");
    scan_font_dir("/mnt/sdcard/Books/fonts");
}
/* A margin on every page, not just the first: without it the text of a
 * continued page starts against the top edge. */
#define PAGE_CSS "@page{margin:0.9em 1.3em}"
static void apply_font(void) {
    if (font_sel < 0 || font_sel >= font_count) font_sel = 0;
    char css[sizeof fonts[0].css + sizeof PAGE_CSS];
    snprintf(css, sizeof css, "%s%s", PAGE_CSS, fonts[font_sel].css);
    fz_set_user_css(ctx, css);
    fz_set_use_document_css(ctx, fonts[font_sel].builtin && fonts[font_sel].css[0] == 0);
}

/* Reading themes. Light passes original colors through (images stay true).
 * Sepia/Dark map each pixel's luminance across a two-tone page->ink ramp, which
 * reads cleanly for text and gives a classic e-reader tint on the whole page. */
enum { TH_LIGHT, TH_SEPIA, TH_DARK, TH_N };
static int theme = TH_LIGHT;
static const char *theme_name[TH_N] = { "Light", "Sepia", "Dark" };
/* ramp[theme] = { page{r,g,b} at luma 255, ink{r,g,b} at luma 0 } */
static const unsigned char ramp[TH_N][2][3] = {
    { {255,255,255}, {  0,  0,  0} },   /* light (mapping skipped) */
    { {244,236,216}, { 91, 64, 40} },   /* sepia: cream page, brown ink */
    { { 24, 24, 26}, {212,212,208} },   /* dark: near-black page, warm grey ink */
};
static uint16_t theme_lut[TH_N][256];
static void themes_init(void) {
    for (int t = TH_SEPIA; t < TH_N; t++) {
        const unsigned char *bg = ramp[t][0], *ink = ramp[t][1];
        for (int L = 0; L < 256; L++) {
            int r = ink[0] + (bg[0]-ink[0])*L/255;
            int g = ink[1] + (bg[1]-ink[1])*L/255;
            int b = ink[2] + (bg[2]-ink[2])*L/255;
            theme_lut[t][L] = rgb565(r, g, b);
        }
    }
}
static uint16_t bg565(void) {
    const unsigned char *p = ramp[theme][0];
    return theme == TH_LIGHT ? 0xFFFF : rgb565(p[0], p[1], p[2]);
}
static uint16_t ink565(void) {
    const unsigned char *p = ramp[theme][1];
    return rgb565(p[0], p[1], p[2]);
}
static void pix_to_fb(const fz_pixmap *pix) {
    int W = pix->w < PW ? pix->w : PW;
    int H = pix->h < reading_height() ? pix->h : reading_height();
    uint16_t fill = bg565();
    for (size_t i = 0; i < (size_t)PW*PH; i++) FB[i] = fill;
    for (int y = 0; y < H; y++) {
        const unsigned char *s = pix->samples + (size_t)y*pix->stride;
        uint16_t *d = FB + (size_t)y*PW + (PW - W)/2;
        for (int x = 0; x < W; x++) {
            int r = s[0], g = s[1], b = s[2];
            if (theme != TH_LIGHT) {
                int L = (r*77 + g*150 + b*29) >> 8;      /* luma 0..255 */
                d[x] = theme_lut[theme][L];
            } else {
                d[x] = rgb565(r, g, b);
            }
            s += pix->n;
        }
    }
}
static void present(void) { hwdisp_present(FB, PW, PH, PW*2); }

static void splash(const char *msg) {
    for (size_t i = 0; i < (size_t)PW*PH; i++) FB[i] = c_panel;
    draw_text_center(PH/2 + (int)(8*S), ui_reg, 20*S, msg, c_text);
    present();
}

static void render_page(void) {
    if (page >= count) page = count - 1;
    if (page < 0) page = 0;
    fz_pixmap *pix = NULL;
    fz_try(ctx) {
        pix = fz_new_pixmap_from_page_number(ctx, doc, page, fz_scale(1,1), rgb, 0);
        pix_to_fb(pix);
    } fz_always(ctx) { if (pix) fz_drop_pixmap(ctx, pix); }
    fz_catch(ctx) { fz_report_error(ctx); memset(FB, 0xFF, (size_t)PW*PH*2); }
}

/* ---- positions that survive a reflow: chapter + fraction of it ------------- */
typedef struct { int ch; float frac; } Spot;

static Spot spot_of(int p) {
    Spot s = { 0, count > 1 ? (float)p / count : 0 };
    fz_var(s);
    fz_try(ctx) {
        fz_location loc = fz_location_from_page_number(ctx, doc, p);
        int n = fz_count_chapter_pages(ctx, doc, loc.chapter);
        s.ch = loc.chapter;
        s.frac = n > 0 ? (float)loc.page / n : 0;
    } fz_catch(ctx) { s.ch = -1; }
    return s;
}
static int page_of(Spot s) {
    int p = -1;
    fz_var(p);
    if (s.ch < 0) return (int)(s.frac * count);
    fz_try(ctx) {
        int n = fz_count_chapter_pages(ctx, doc, s.ch);
        int cp = (int)(s.frac * n + 0.001f);
        if (cp >= n) cp = n - 1;
        if (cp < 0) cp = 0;
        p = fz_page_number_from_location(ctx, doc, fz_make_location(s.ch, cp));
    } fz_catch(ctx) { p = -1; }
    if (p < 0) p = 0;
    if (p >= count) p = count - 1;
    return p;
}

/* ---- chapters (the book's contents) ---------------------------------------- */
typedef struct { char title[128]; int level, page; } Chapter;
static Chapter *chapters;
static int nchapters, chapters_cap;

static void add_chapter(const char *title, int level, int p) {
    if (p < 0 || p >= count) return;
    if (nchapters == chapters_cap) {
        int cap = chapters_cap ? chapters_cap * 2 : 64;
        Chapter *c = realloc(chapters, (size_t)cap * sizeof *c);
        if (!c) return;
        chapters = c; chapters_cap = cap;
    }
    Chapter *c = &chapters[nchapters++];
    const char *t = title ? title : "";
    while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
    snprintf(c->title, sizeof c->title, "%s", *t ? t : "Untitled");
    for (char *q = c->title; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    c->level = level;
    c->page = p;
}
static int outline_page(fz_outline *o) {
    int p = -1;
    fz_var(p);
    fz_try(ctx) {
        fz_location loc = o->page;
        if (loc.chapter < 0 && o->uri) loc = fz_resolve_link(ctx, doc, o->uri, NULL, NULL);
        if (loc.chapter >= 0) p = fz_page_number_from_location(ctx, doc, loc);
    } fz_catch(ctx) { p = -1; }
    return p;
}
static void add_outline(fz_outline *o, int level) {
    for (; o; o = o->next) {
        add_chapter(o->title, level, outline_page(o));
        if (o->down && level < 3) add_outline(o->down, level + 1);
    }
}
static fz_outline *load_outline(void) {
    fz_outline *o = NULL;
    fz_var(o);
    fz_try(ctx) o = fz_load_outline(ctx, doc);
    fz_catch(ctx) o = NULL;
    return o;
}
static int count_sections(void) {
    int n = 0;
    fz_var(n);
    fz_try(ctx) n = fz_count_chapters(ctx, doc);
    fz_catch(ctx) n = 0;
    return n;
}
static int section_page(int i) {
    int p = -1;
    fz_var(p);
    fz_try(ctx) p = fz_page_number_from_location(ctx, doc, fz_make_location(i, 0));
    fz_catch(ctx) p = -1;
    return p;
}
static void chapters_load(void) {
    nchapters = 0;
    fz_outline *o = load_outline();
    if (o) { add_outline(o, 0); fz_drop_outline(ctx, o); }
    if (nchapters == 0) {
        /* No contents: the book's own sections (EPUB spine files). */
        int n = count_sections();
        for (int i = 0; n > 1 && i < n && i < 1000; i++) {
            char t[32];
            snprintf(t, sizeof t, "Section %d", i + 1);
            add_chapter(t, 0, section_page(i));
        }
    }
}
/* The chapter p is in: the last entry starting at or before p. -1 if none. */
static int chapter_at(int p) {
    int best = -1;
    for (int i = 0; i < nchapters; i++)
        if (chapters[i].page <= p && (best < 0 || chapters[i].page >= chapters[best].page)) best = i;
    return best;
}
static int next_chapter_page(int p) {
    int best = -1;
    for (int i = 0; i < nchapters; i++)
        if (chapters[i].page > p && (best < 0 || chapters[i].page < best)) best = chapters[i].page;
    return best;
}
/* Start of this chapter, or of the previous one when already at its start. */
static int prev_chapter_page(int p) {
    int best = -1;
    for (int i = 0; i < nchapters; i++)
        if (chapters[i].page < p && chapters[i].page > best) best = chapters[i].page;
    return best;
}

/* ---- bookmarks --------------------------------------------------------------- */
#define MAX_MARKS 200
static Spot marks[MAX_MARKS];
static int mark_page[MAX_MARKS];
static int nmarks;

static void marks_pages(void) {
    for (int i = 0; i < nmarks; i++) mark_page[i] = page_of(marks[i]);
    /* Keep them in reading order. */
    for (int i = 1; i < nmarks; i++)
        for (int j = i; j > 0 && mark_page[j-1] > mark_page[j]; j--) {
            Spot s = marks[j]; marks[j] = marks[j-1]; marks[j-1] = s;
            int t = mark_page[j]; mark_page[j] = mark_page[j-1]; mark_page[j-1] = t;
        }
}
static int mark_on(int p) {
    for (int i = 0; i < nmarks; i++) if (mark_page[i] == p) return i;
    return -1;
}

static void reflow(void) {
    splash("Reflowing...");
    Spot here = spot_of(page);
    fz_try(ctx) {
        fz_layout_document(ctx, doc, (float)PW, (float)reading_height(), em_from_px());
        count = fz_count_pages(ctx, doc);
    } fz_catch(ctx) { fz_report_error(ctx); }
    page = page_of(here);
    chapters_load();
    marks_pages();
}

/* ---- progress persistence -------------------------------------------------- */
static char pos_path[1100];
static char legacy_pos_path[1100];
static char marks_path[1100];
static int pos_dirty;
static uint64_t pos_changed_ms, pos_last_save_ms;
static Spot pos_spot;       /* from the position file; ch -1 when absent */
static int pos_has_spot;

static void pos_mark_dirty(void) {
    pos_dirty = 1;
    pos_changed_ms = ticks_ms();
}
static void pos_init(const char *book) {
    const char *base = strrchr(book, '/');
    size_t dir_len = base ? (size_t)(base - book) : 0;
    base = base ? base + 1 : book;

    snprintf(legacy_pos_path, sizeof legacy_pos_path, "%s.pos", book);
    if (dir_len > 0)
        snprintf(pos_path, sizeof pos_path, "%.*s/.positions/%s.pos",
                 (int)dir_len, book, base);
    else
        snprintf(pos_path, sizeof pos_path, ".positions/%s.pos", base);

    char dir[1100];
    if (dir_len > 0)
        snprintf(dir, sizeof dir, "%.*s/.positions", (int)dir_len, book);
    else
        snprintf(dir, sizeof dir, ".positions");

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        /* Read-only or otherwise unusable directory: retain the old location
         * so progress still works, even though it cannot be hidden. */
        snprintf(pos_path, sizeof pos_path, "%s", legacy_pos_path);
    } else if (access(pos_path, F_OK) != 0 && access(legacy_pos_path, F_OK) == 0) {
        /* Move an existing sidecar out of the book list. Same-filesystem
         * rename is atomic. pos_load() still falls back to it if migration
         * is not possible. */
        rename(legacy_pos_path, pos_path);
    }
    snprintf(marks_path, sizeof marks_path, "%.*s.marks",
             (int)(strlen(pos_path) - 4), pos_path);
}
/* "page px font theme [chapter fraction]"; the last two came with
 * ShallotsOS and place the page exactly after a reflow. */
static void pos_load(void) {
    FILE *f = fopen(pos_path, "r");
    if (!f && strcmp(pos_path, legacy_pos_path) != 0)
        f = fopen(legacy_pos_path, "r");
    if (!f) return;
    int p, px = 0, fs = 0, th = 0, ch = -1;
    float fr = 0;
    int n = fscanf(f, "%d %d %d %d %d %f", &p, &px, &fs, &th, &ch, &fr);
    if (n >= 1) page = p;
    if (n >= 2 && px >= PX_MIN && px <= PX_MAX) font_px = px;
    if (n >= 3 && fs >= 0 && fs < font_count) font_sel = fs;
    if (n >= 4 && th >= 0 && th < TH_N) theme = th;
    if (n >= 6 && ch >= 0 && fr >= 0 && fr < 1) { pos_spot.ch = ch; pos_spot.frac = fr; pos_has_spot = 1; }
    fclose(f);
}
static int write_atomic(const char *path, const char *text) {
    char tmp[1110];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}
static void pos_save(void) {
    char line[96];
    Spot s = doc ? spot_of(page) : (Spot){ -1, 0 };
    if (s.ch >= 0)
        snprintf(line, sizeof line, "%d %d %d %d %d %.6f\n", page, font_px, font_sel, theme, s.ch, s.frac);
    else
        snprintf(line, sizeof line, "%d %d %d %d\n", page, font_px, font_sel, theme);
    if (!write_atomic(pos_path, line)) return;
    if (strcmp(pos_path, legacy_pos_path) != 0)
        unlink(legacy_pos_path);
    pos_dirty = 0;
    pos_last_save_ms = ticks_ms();
}
static void marks_load(void) {
    nmarks = 0;
    FILE *f = fopen(marks_path, "r");
    if (!f) return;
    int ch; float fr;
    while (nmarks < MAX_MARKS && fscanf(f, "%d %f", &ch, &fr) == 2)
        if (fr >= 0 && fr < 1) { marks[nmarks].ch = ch; marks[nmarks].frac = fr; nmarks++; }
    fclose(f);
}
static void marks_save(void) {
    if (nmarks == 0) { unlink(marks_path); return; }
    char *buf = malloc((size_t)nmarks * 32 + 1);
    if (!buf) return;
    size_t o = 0;
    for (int i = 0; i < nmarks; i++)
        o += (size_t)snprintf(buf + o, 32, "%d %.6f\n", marks[i].ch, marks[i].frac);
    buf[o] = 0;
    write_atomic(marks_path, buf);
    free(buf);
}
/* Returns 1 when one was added, 0 when removed, -1 when full. */
static int mark_toggle(int p) {
    int i = mark_on(p);
    if (i >= 0) {
        memmove(&marks[i], &marks[i+1], (size_t)(nmarks - i - 1) * sizeof *marks);
        memmove(&mark_page[i], &mark_page[i+1], (size_t)(nmarks - i - 1) * sizeof *mark_page);
        nmarks--;
        marks_save();
        return 0;
    }
    if (nmarks >= MAX_MARKS) return -1;
    marks[nmarks] = spot_of(p);
    mark_page[nmarks] = p;
    nmarks++;
    marks_pages();
    marks_save();
    return 1;
}

/* ---- reading screen ---------------------------------------------------------- */
static char book_title[160];
static char toast_msg[64];
static uint64_t toast_until;

static void toast(const char *msg) {
    snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_until = ticks_ms() + 1400;
}

/* A small ribbon at the top right of a bookmarked page. */
static void draw_ribbon(void) {
    int w = (int)(14*S), h = (int)(24*S), x = PW - w - (int)(16*S);
    for (int j = 0; j < h; j++) {
        int notch = j > h - w/2 ? (j - (h - w/2)) : 0;
        for (int i = 0; i < w; i++) {
            if (notch && i >= w/2 - notch && i < w/2 + notch) continue;
            FB[j*PW + x + i] = c_sel;
        }
    }
}

/* The footer: chapter on the left, page and percent on the right, in the
 * page's own colours like an e-reader. */
static void draw_footer(void) {
    int y = PH - FOOTER_H;
    uint16_t bg = bg565(), ink = ink565(), dim = mix565(ink, bg, 120);
    fill_rect(0, y, PW, FOOTER_H, bg);
    fill_rect((int)(16*S), y, PW - (int)(32*S), S >= 1.5f ? 2 : 1, mix565(ink, bg, 210));
    char right[48];
    int pct = count > 1 ? page*100/(count-1) : 100;
    snprintf(right, sizeof right, "%d / %d   %d%%", page+1, count, pct);
    float fs = 14*S;
    int base = y + FOOTER_H/2 + (int)(fs*0.36f);
    int rw = text_w(ui_reg, fs, right);
    draw_text_right(PW - (int)(16*S), base, ui_reg, fs, right, dim);
    int c = chapter_at(page);
    const char *left = c >= 0 ? chapters[c].title : book_title;
    draw_text((int)(16*S), base, ui_reg, fs, left, dim, PW - rw - (int)(56*S));
}

static void draw_toast(void) {
    if (!toast_msg[0] || ticks_ms() >= toast_until) return;
    float fs = 17*S;
    int tw = text_w(ui_reg, fs, toast_msg), h = (int)(40*S), w = tw + (int)(40*S);
    int x = (PW - w) / 2, y = PH - FOOTER_H - h - (int)(14*S);
    fill_round(x, y, w, h, h/2, 0x2104);
    draw_text(x + (w - tw)/2, y + h/2 + (int)(fs*0.36f), ui_reg, fs, toast_msg, 0xFFFF, 0);
}

static void draw_reading(void) {
    render_page();
    if (mark_on(page) >= 0) draw_ribbon();
    draw_footer();
    draw_toast();
    present();
}

/* ---- menu and lists, in the ShallotsOS theme ----------------------------------- */
#define ROW_H ((int)(36*S))
#define LIST_FS (17*S)

static void draw_header(const char *title) {
    int h = (int)(48*S);
    fill_rect(0, 0, PW, h, mix565(c_panel, 0x0000, 60));
    draw_text((int)(20*S), h/2 + (int)(20*S*0.36f), ui_bold, 20*S, title, c_text, PW - (int)(40*S));
}
static void draw_hints(const char *hints) {
    int h = (int)(40*S), y = PH - h;
    fill_rect(0, y, PW, h, mix565(c_panel, 0x0000, 60));
    draw_text((int)(20*S), y + h/2 + (int)(14*S*0.36f), ui_reg, 14*S, hints, c_dim, PW - (int)(40*S));
}

/* A scrolling list: rows of (label, right-hand value). Returns the chosen
 * row, -1 for back. del_key: called with X on a row (may change n). */
typedef const char *(*row_fn)(int i, char *right, size_t n, int *indent);
static int list_loop(const char *title, int *n, int sel, row_fn row, const char *hints,
                     const char *empty, int (*del_row)(int)) {
    int top = 0, redraw = 1;
    int head = (int)(48*S), foot = (int)(40*S);
    int vis = (PH - head - foot - (int)(16*S)) / ROW_H;
    if (vis < 1) vis = 1;
    uint32_t prev = keys();
    uint64_t held = 0, rep = 0;
    for (;;) {
        if (sel >= *n) sel = *n - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        if (redraw) {
            redraw = 0;
            for (size_t i = 0; i < (size_t)PW*PH; i++) FB[i] = c_panel;
            draw_header(title);
            if (*n == 0) draw_text_center(PH/2, ui_reg, LIST_FS, empty, c_dim);
            for (int i = top; i < *n && i < top + vis; i++) {
                int y = head + (int)(8*S) + (i - top) * ROW_H;
                int x = (int)(12*S), w = PW - (int)(24*S);
                if (i == sel) fill_round(x, y, w, ROW_H - (int)(4*S), (int)(10*S), c_sel);
                char right[48] = "";
                int indent = 0;
                const char *label = row(i, right, sizeof right, &indent);
                uint16_t tc = i == sel ? c_sel_text : c_text;
                int base = y + (ROW_H - (int)(4*S))/2 + (int)(LIST_FS*0.36f);
                int rw = right[0] ? text_w(ui_reg, LIST_FS, right) + (int)(16*S) : 0;
                if (right[0]) draw_text_right(x + w - (int)(14*S), base, ui_reg, LIST_FS, right,
                                              i == sel ? c_sel_text : c_dim);
                int lx = x + (int)(14*S) + indent * (int)(18*S);
                draw_text(lx, base, ui_reg, LIST_FS, label, tc, x + w - (int)(14*S) - rw - lx);
            }
            if (*n > vis) {   /* scroll bar */
                int th = PH - head - foot - (int)(16*S), bh = th * vis / *n;
                if (bh < (int)(12*S)) bh = (int)(12*S);
                int by = head + (int)(8*S) + (th - bh) * top / (*n - vis);
                fill_round(PW - (int)(7*S), by, (int)(4*S), bh, (int)(2*S), c_dim);
            }
            draw_hints(hints);
            present();
        }
        uint32_t k = keys(), pr = pressed(&prev, &held, &rep);
        if ((k & BIT(K_SELECT)) && (k & BIT(K_START))) return -2;
        if (pr & BIT(K_UP))   { sel = sel > 0 ? sel - 1 : *n - 1; redraw = 1; }
        if (pr & BIT(K_DOWN)) { sel = sel < *n - 1 ? sel + 1 : 0; redraw = 1; }
        if (pr & (BIT(K_LEFT)|BIT(K_L1))) { sel -= vis; if (sel < 0) sel = 0; redraw = 1; }
        if (pr & (BIT(K_RIGHT)|BIT(K_R1))) { sel += vis; if (sel >= *n) sel = *n - 1; redraw = 1; }
        if ((pr & BIT(K_X)) && del_row && *n > 0) { del_row(sel); redraw = 1; }
        if (pr & (BIT(K_B)|BIT(K_SELECT))) return -1;
        if ((pr & BIT(K_A)) && *n > 0) return sel;
        usleep(15000);
    }
}

static const char *chapter_row(int i, char *right, size_t n, int *indent) {
    snprintf(right, n, "%d", chapters[i].page + 1);
    *indent = chapters[i].level;
    return chapters[i].title;
}
/* Returns 1 when the page changed, 2 to quit. */
static int contents_screen(void) {
    int c = chapter_at(page);
    int r = list_loop("Contents", &nchapters, c < 0 ? 0 : c, chapter_row,
                      "A  Go     B  Back     L1/R1  Page", "This book has no contents.", NULL);
    if (r == -2) return 2;
    if (r < 0) return 0;
    page = chapters[r].page;
    pos_mark_dirty();
    return 1;
}

static char mark_label[160];
static const char *mark_row(int i, char *right, size_t n, int *indent) {
    int p = mark_page[i], c = chapter_at(p);
    int pct = count > 1 ? p*100/(count-1) : 100;
    snprintf(right, n, "%d   %d%%", p + 1, pct);
    *indent = 0;
    snprintf(mark_label, sizeof mark_label, "%s", c >= 0 ? chapters[c].title : "Page");
    return mark_label;
}
static int mark_delete(int i) {
    if (i < 0 || i >= nmarks) return 0;
    mark_toggle(mark_page[i]);
    return 1;
}
static int bookmarks_screen(void) {
    int sel = 0;
    for (int i = 0; i < nmarks; i++) if (mark_page[i] <= page) sel = i;
    int r = list_loop("Bookmarks", &nmarks, sel, mark_row,
                      "A  Go     X  Delete     B  Back",
                      "No bookmarks yet. Press Y while reading to add one.", mark_delete);
    if (r == -2) return 2;
    if (r < 0) return 0;
    page = mark_page[r];
    pos_mark_dirty();
    return 1;
}

/* ---- menu ------------------------------------------------------------------
 * Two row kinds:
 *   VALUE rows (Text size, Font, Theme) -> LEFT/RIGHT change the value.
 *   ACTION rows -> A activates.
 */
enum { M_RESUME, M_CONTENTS, M_BOOKMARKS, M_MARK, M_SIZE, M_FONT, M_THEME, M_START, M_END, M_QUIT, M_N };

static const char *menu_label(int i, char *right, size_t n) {
    right[0] = 0;
    switch (i) {
    case M_RESUME:    return "Resume";
    case M_CONTENTS:  snprintf(right, n, "%d", nchapters); return "Contents";
    case M_BOOKMARKS: snprintf(right, n, "%d", nmarks); return "Bookmarks";
    case M_MARK:      return mark_on(page) >= 0 ? "Remove bookmark here" : "Bookmark this page";
    case M_SIZE:      snprintf(right, n, "<  %d px  >", font_px); return "Text size";
    case M_FONT:      snprintf(right, n, "<  %s  >", fonts[font_sel].name); return "Font";
    case M_THEME:     snprintf(right, n, "<  %s  >", theme_name[theme]); return "Theme";
    case M_START:     return "Go to start";
    case M_END:       return "Go to end";
    case M_QUIT:      return "Quit";
    default:          return "";
    }
}

/* returns: 0 stay, 1 quit */
static int menu_loop(void) {
    int sel = 0, redraw = 1, dirty = 0;   /* dirty: size/font changed, reflow on exit */
    /* The page, dimmed, is the backdrop; size/font changes only reflow when
     * you LEAVE the menu, not per keypress. */
    uint16_t *bg = malloc((size_t)PW*PH*2);
    if (!bg) return 1;
    render_page();
    draw_footer();
    dim_rect(0, 0, PW, PH, 150);
    memcpy(bg, FB, (size_t)PW*PH*2);
    #define MENU_CLOSE()  do { if (dirty) reflow(); free(bg); return 0; } while (0)
    uint32_t prev = keys();
    uint64_t held = 0, rep = 0;
    for (;;) {
        if (redraw) {
            redraw = 0;
            memcpy(FB, bg, (size_t)PW*PH*2);
            int head = (int)(56*S), foot = (int)(34*S);
            int bw = PW - (int)(80*S), bh = head + M_N * ROW_H + foot + (int)(8*S);
            if (bh > PH - (int)(16*S)) bh = PH - (int)(16*S);
            int bx = (PW - bw) / 2, by = (PH - bh) / 2, r = (int)(16*S);
            fill_round(bx, by, bw, bh, r, c_panel);
            draw_text(bx + (int)(22*S), by + (int)(34*S), ui_bold, 19*S, book_title, c_text, bw - (int)(44*S));
            int rows = (bh - head - foot - (int)(8*S)) / ROW_H;
            int top = sel >= rows ? sel - rows + 1 : 0;
            for (int i = top; i < M_N && i < top + rows; i++) {
                int y = by + head + (i - top) * ROW_H;
                int x = bx + (int)(10*S), w = bw - (int)(20*S);
                if (i == sel) fill_round(x, y, w, ROW_H - (int)(4*S), (int)(10*S), c_sel);
                char right[64];
                const char *lbl = menu_label(i, right, sizeof right);
                int base = y + (ROW_H - (int)(4*S))/2 + (int)(LIST_FS*0.36f);
                if (right[0]) draw_text_right(x + w - (int)(14*S), base, ui_reg, LIST_FS, right,
                                              i == sel ? c_sel_text : c_dim);
                draw_text(x + (int)(14*S), base, ui_reg, LIST_FS, lbl, i == sel ? c_sel_text : c_text,
                          w / 2);
            }
            draw_text(bx + (int)(22*S), by + bh - foot/2 + (int)(13*S*0.36f), ui_reg, 13*S,
                      "A  Select     B  Back     Left/Right  Change", c_dim, bw - (int)(44*S));
            present();
        }

        uint32_t k = keys(), pr = pressed(&prev, &held, &rep);
        if ((k & BIT(K_SELECT)) && (k & BIT(K_START))) { free(bg); pos_save(); return 1; }
        if (pr & BIT(K_UP))   { sel = sel > 0 ? sel - 1 : M_N - 1; redraw = 1; }
        if (pr & BIT(K_DOWN)) { sel = sel < M_N - 1 ? sel + 1 : 0; redraw = 1; }

        /* LEFT/RIGHT adjust value rows in place - value + label update now, the
         * (slow) reflow is deferred until the menu closes. */
        if (pr & (BIT(K_LEFT)|BIT(K_RIGHT))) {
            int dir = (pr & BIT(K_RIGHT)) ? 1 : -1;
            if (sel == M_SIZE) {
                int np = font_px + dir*2;
                if (np >= PX_MIN && np <= PX_MAX) {
                    font_px = np; dirty = 1; redraw = 1; pos_mark_dirty();
                }
            } else if (sel == M_FONT) {
                font_sel = (font_sel + dir + font_count) % font_count;
                apply_font(); dirty = 1; redraw = 1; pos_mark_dirty();
            } else if (sel == M_THEME) {
                theme = (theme + dir + TH_N) % TH_N;   /* color only: no reflow */
                render_page(); draw_footer(); dim_rect(0, 0, PW, PH, 150);
                memcpy(bg, FB, (size_t)PW*PH*2);
                redraw = 1; pos_mark_dirty();
            }
        }

        if (pr & (BIT(K_SELECT)|BIT(K_B))) MENU_CLOSE();
        if (pr & BIT(K_A)) {
            int r;
            switch (sel) {
            case M_RESUME: MENU_CLOSE();
            case M_CONTENTS:
            case M_BOOKMARKS:
                if (dirty) { reflow(); dirty = 0; }
                r = sel == M_CONTENTS ? contents_screen() : bookmarks_screen();
                if (r == 2) { free(bg); pos_save(); return 1; }
                if (r == 1) MENU_CLOSE();
                prev = keys(); redraw = 1;
                break;
            case M_MARK:
                if (dirty) { reflow(); dirty = 0; }
                r = mark_toggle(page);
                toast(r > 0 ? "Bookmark added" : r == 0 ? "Bookmark removed" : "Too many bookmarks");
                MENU_CLOSE();
            case M_START: page = 0; pos_mark_dirty(); MENU_CLOSE();
            case M_END:   page = count-1; pos_mark_dirty(); MENU_CLOSE();
            case M_QUIT:  free(bg); pos_save(); return 1;
            default: break;   /* value rows: A does nothing */
            }
        }
        usleep(15000);
    }
}

static void title_init(const char *path) {
    book_title[0] = 0;
    fz_try(ctx) fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, book_title, sizeof book_title);
    fz_catch(ctx) book_title[0] = 0;
    if (!book_title[0]) {
        const char *b = strrchr(path, '/');
        snprintf(book_title, sizeof book_title, "%s", b ? b + 1 : path);
        char *dot = strrchr(book_title, '.');
        if (dot && dot != book_title) *dot = 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: reader <book>\n"); return 1; }
    const char *path = argv[1];

    canvas_init();

    if (hwdisp_init() != 0) { fprintf(stderr, "hwdisp_init failed\n"); return 1; }
    hwdisp_set_target_aspect(0, 0);    /* passthrough: hand the driver the canvas */
    hwdisp_set_filter(0);              /* HW bilinear scale to the panel */
    input_init();

    FB = malloc((size_t)PW*PH*2);
    if (!FB) return 1;

    ctx = fz_new_context(NULL, NULL, EBOOK_STORE_BYTES);
    if (!ctx) return 1;
    rgb = fz_device_rgb(ctx);
    colours_init();
    ui_init();
    themes_init();
    fonts_init();
    splash("Loading...");
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, path);
        pos_init(path);
        pos_load();                         /* may set font_px + font_sel first */
        apply_font();
        fz_layout_document(ctx, doc, (float)PW, (float)reading_height(), em_from_px());
        count = fz_count_pages(ctx, doc);
    } fz_catch(ctx) {
        fz_report_error(ctx);
        splash("Cannot open book"); usleep(1500000);
        return 1;
    }
    if (pos_has_spot) page = page_of(pos_spot);
    if (page >= count) page = count-1;
    if (page < 0) page = 0;
    title_init(path);
    chapters_load();
    marks_load();
    marks_pages();

    int need_draw = 1;
    uint32_t prev = keys();
    uint64_t held = 0, rep = 0;
    for (;;) {
        if (need_draw) { need_draw = 0; draw_reading(); }
        if (toast_until && ticks_ms() >= toast_until) { toast_until = 0; toast_msg[0] = 0; need_draw = 1; }

        uint32_t k = keys(), pr = pressed(&prev, &held, &rep);

        if ((k & BIT(K_SELECT)) && (k & BIT(K_START))) { pos_save(); break; }

        if (pr & BIT(K_SELECT)) {
            if (menu_loop()) break;         /* quit from menu */
            need_draw = 1; prev = keys();
            continue;
        }
        if (pr & BIT(K_X)) {
            if (contents_screen() == 2) { pos_save(); break; }
            need_draw = 1; prev = keys();
            continue;
        }
        int moved = 0;
        if (pr & (BIT(K_R1)|BIT(K_RIGHT)|BIT(K_A))) { if (page<count-1){ page++; moved=1; } }
        if (pr & (BIT(K_L1)|BIT(K_LEFT)|BIT(K_B)))  { if (page>0){ page--; moved=1; } }
        if (pr & BIT(K_R2)) { page += count/10 + 1; if (page>=count) page=count-1; moved=1; }
        if (pr & BIT(K_L2)) { page -= count/10 + 1; if (page<0) page=0; moved=1; }
        if (pr & BIT(K_DOWN)) {
            int p = next_chapter_page(page);
            if (p >= 0) { page = p; moved = 1; } else toast("Last chapter");
            need_draw = 1;
        }
        if (pr & BIT(K_UP)) {
            int p = prev_chapter_page(page);
            if (p >= 0) { page = p; moved = 1; } else if (page > 0) { page = 0; moved = 1; }
        }
        if (pr & BIT(K_Y)) {
            int r = mark_toggle(page);
            toast(r > 0 ? "Bookmark added" : r == 0 ? "Bookmark removed" : "Too many bookmarks");
            need_draw = 1;
        }
        if (moved) { need_draw = 1; pos_mark_dirty(); }

        /* Do not write to the FAT card while the user is flipping pages. Save
         * after navigation settles, and never more than once every five seconds. */
        uint64_t now = ticks_ms();
        if (pos_dirty && now - pos_changed_ms >= 5000 &&
            now - pos_last_save_ms >= 5000)
            pos_save();

        usleep(15000);
    }

    /* Fast exit: fz_drop_document/context walks and frees the whole font/page
     * store, which is SLOW on this CPU for a big epub. We're quitting anyway, so
     * save progress, release the display, and let the kernel reclaim memory. */
    splash("Exiting...");
    pos_save();
    hwdisp_deinit();
    _exit(0);
}
