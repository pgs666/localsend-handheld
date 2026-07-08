#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#define FB_WIDTH 960
#define FB_HEIGHT 544
#define GLYPH_WIDTH 5
#define GLYPH_HEIGHT 7

static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT];

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static const uint8_t* glyph_for(char c) {
  static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
  static const uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
  static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0};
  static const uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
  static const uint8_t zero[7] = {14, 17, 19, 21, 25, 17, 14};
  static const uint8_t one[7] = {4, 12, 4, 4, 4, 4, 14};
  static const uint8_t two[7] = {14, 17, 1, 2, 4, 8, 31};
  static const uint8_t a[7] = {14, 17, 17, 31, 17, 17, 17};
  static const uint8_t b[7] = {30, 17, 17, 30, 17, 17, 30};
  static const uint8_t c_[7] = {14, 17, 16, 16, 16, 17, 14};
  static const uint8_t d[7] = {30, 17, 17, 17, 17, 17, 30};
  static const uint8_t e[7] = {31, 16, 16, 30, 16, 16, 31};
  static const uint8_t f[7] = {31, 16, 16, 30, 16, 16, 16};
  static const uint8_t g[7] = {14, 17, 16, 23, 17, 17, 14};
  static const uint8_t h[7] = {17, 17, 17, 31, 17, 17, 17};
  static const uint8_t i[7] = {14, 4, 4, 4, 4, 4, 14};
  static const uint8_t k[7] = {17, 18, 20, 24, 20, 18, 17};
  static const uint8_t l[7] = {16, 16, 16, 16, 16, 16, 31};
  static const uint8_t n[7] = {17, 25, 21, 19, 17, 17, 17};
  static const uint8_t o[7] = {14, 17, 17, 17, 17, 17, 14};
  static const uint8_t p[7] = {30, 17, 17, 30, 16, 16, 16};
  static const uint8_t r[7] = {30, 17, 17, 30, 20, 18, 17};
  static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
  static const uint8_t t[7] = {31, 4, 4, 4, 4, 4, 4};
  static const uint8_t u[7] = {17, 17, 17, 17, 17, 17, 14};
  static const uint8_t v[7] = {17, 17, 17, 17, 17, 10, 4};
  static const uint8_t x[7] = {17, 17, 10, 4, 10, 17, 17};
  static const uint8_t y[7] = {17, 17, 10, 4, 4, 4, 4};

  if (c >= 'a' && c <= 'z') {
    c = (char)(c - 'a' + 'A');
  }

  switch (c) {
  case '0': return zero;
  case '1': return one;
  case '2': return two;
  case ':': return colon;
  case '-': return dash;
  case '.': return dot;
  case 'A': return a;
  case 'B': return b;
  case 'C': return c_;
  case 'D': return d;
  case 'E': return e;
  case 'F': return f;
  case 'G': return g;
  case 'H': return h;
  case 'I': return i;
  case 'K': return k;
  case 'L': return l;
  case 'N': return n;
  case 'O': return o;
  case 'P': return p;
  case 'R': return r;
  case 'S': return s;
  case 'T': return t;
  case 'U': return u;
  case 'V': return v;
  case 'X': return x;
  case 'Y': return y;
  default: return blank;
  }
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
  for (int yy = 0; yy < h; ++yy) {
    const int py = y + yy;
    if (py < 0 || py >= FB_HEIGHT) {
      continue;
    }
    for (int xx = 0; xx < w; ++xx) {
      const int px = x + xx;
      if (px >= 0 && px < FB_WIDTH) {
        framebuffer[py * FB_WIDTH + px] = color;
      }
    }
  }
}

static void draw_char(int x, int y, char c, uint32_t color, int scale) {
  const uint8_t* glyph = glyph_for(c);
  for (int row = 0; row < GLYPH_HEIGHT; ++row) {
    for (int col = 0; col < GLYPH_WIDTH; ++col) {
      if (glyph[row] & (1u << (GLYPH_WIDTH - 1 - col))) {
        fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static void draw_text(int x, int y, const char* text, uint32_t color, int scale) {
  const int advance = (GLYPH_WIDTH + 1) * scale;
  while (*text) {
    draw_char(x, y, *text, color, scale);
    x += advance;
    ++text;
  }
}

static void render_screen(int frames) {
  const uint32_t background = rgb(13, 18, 32);
  const uint32_t panel = rgb(24, 36, 56);
  const uint32_t accent = rgb(29, 185, 140);
  const uint32_t text = rgb(232, 238, 247);
  const uint32_t muted = rgb(143, 156, 176);

  for (int i = 0; i < FB_WIDTH * FB_HEIGHT; ++i) {
    framebuffer[i] = background;
  }

  fill_rect(64, 64, 832, 416, panel);
  fill_rect(64, 64, 832, 8, accent);
  fill_rect(96, 120, 96, 96, accent);
  draw_text(116, 147, "LS", background, 6);
  draw_text(224, 124, "LOCALSEND HANDHELD", text, 5);
  draw_text(224, 184, "PS VITA SMOKE UI", muted, 3);
  draw_text(96, 288, "PROTOCOL CORE IS NOT WIRED ON PSV YET", text, 3);
  draw_text(96, 336, "START: EXIT", muted, 3);

  const int pulse = 96 + ((frames / 20) % 20) * 8;
  fill_rect(pulse, 424, 128, 8, accent);

  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  fb.base = framebuffer;
  fb.pitch = FB_WIDTH;
  fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  fb.width = FB_WIDTH;
  fb.height = FB_HEIGHT;
  sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

int main(void) {
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  int frames = 0;
  while (1) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) {
      break;
    }
    render_screen(frames++);
    sceDisplayWaitVblankStart();
    sceKernelDelayThread(1000);
  }

  sceKernelExitProcess(0);
  return 0;
}
