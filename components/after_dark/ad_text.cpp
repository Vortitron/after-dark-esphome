#include "ad_text.h"

#include <cstring>

namespace adcore {

void TextScreen::begin(int cols, int rows) {
  cols_ = cols < MAX_COLS ? cols : MAX_COLS;
  rows_ = rows < MAX_ROWS ? rows : MAX_ROWS;
  clear();
  memset(sent_, 0, sizeof sent_);  // nothing matches, so the first frame goes out whole
  memset(glyphs_, 0, sizeof glyphs_);
  glyph_dirty_ = 0xFF;
}

void TextScreen::clear() {
  memset(chars_, ' ', sizeof chars_);
  memset(alt_, ' ', sizeof alt_);
}

bool TextScreen::any_dim() const {
  for (int r = 0; r < rows_; r++)
    for (int c = 0; c < cols_; c++)
      if (alt_[r][c] != chars_[r][c])
        return true;
  return false;
}

bool TextScreen::changed() const { return glyph_dirty_ || memcmp(chars_, sent_, sizeof chars_) != 0; }

void TextScreen::mark_sent() {
  memcpy(sent_, chars_, sizeof chars_);
  glyph_dirty_ = 0;
}

void TextScreen::invalidate() { memset(sent_, 0, sizeof sent_); }

void TextScreen::put(int col, int row, uint8_t code, bool dim) {
  if (col < 0 || row < 0 || col >= cols_ || row >= rows_)
    return;
  chars_[row][col] = code;
  alt_[row][col] = dim ? ' ' : code;
}

void TextScreen::blend(int col, int row, uint8_t code, uint8_t other) {
  if (col < 0 || row < 0 || col >= cols_ || row >= rows_)
    return;
  chars_[row][col] = code;
  alt_[row][col] = other;
}

void TextScreen::glyph(int slot, const uint8_t bits[8]) {
  if (memcmp(glyphs_[slot], bits, 8) == 0)
    return;
  memcpy(glyphs_[slot], bits, 8);
  glyph_dirty_ |= 1 << slot;
}

void glyph_from_art(const char *const art[8], int col, uint8_t out[8]) {
  for (int y = 0; y < 8; y++) {
    out[y] = 0;
    for (int x = 0; x < 5; x++)
      if (art[y][col + x] == '#')
        out[y] |= 0x10 >> x;
  }
}

void DotMatrix::begin(int cols, int rows) {
  cols_ = cols < TextScreen::MAX_COLS ? cols : TextScreen::MAX_COLS;
  rows_ = rows < TextScreen::MAX_ROWS ? rows : TextScreen::MAX_ROWS;
  memset(cells_, 0, sizeof cells_);
  memset(bright_, 0, sizeof bright_);
  memset(dim_, 0, sizeof dim_);
}

void DotMatrix::pixel(int x, int y, Rgb c) {
  int dx = x % 6, dy = y % 9;
  if (dx == 5 || dy == 8)
    return;  // the gap between cells
  const uint8_t bit = 0x10 >> dx;
  const int lum = mono_lum(c);
  uint8_t &row = cells_[y / 9][x / 6][dy];
  if (mono_on_lum(x, y, lum))
    row |= bit;
  else
    row &= ~bit;
  uint8_t &hi = bright_[y / 9][x / 6][dy], &lo = dim_[y / 9][x / 6][dy];
  hi &= ~bit;
  lo &= ~bit;
  if (lum >= 160)
    hi |= bit;
  else if (lum >= 40)
    lo |= bit;
}

namespace {

/* A few HD44780 ROM characters that stand in for a cell with a dot or two in
   it once the eight glyphs are spoken for. Bitmaps as the A00 ROM has them. */
struct Stand {
  uint8_t code;
  uint8_t bits[8];
};
const Stand STANDS[] = {
    {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C, 0}},          {'\'', {0x0C, 0x04, 0x08, 0, 0, 0, 0, 0}},
    {'-', {0, 0, 0, 0x1F, 0, 0, 0, 0}},             {',', {0, 0, 0, 0, 0, 0x0C, 0x04, 0x08}},
    {'`', {0x08, 0x04, 0x02, 0, 0, 0, 0, 0}},       {':', {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0, 0}},
    {'*', {0, 0x04, 0x15, 0x0E, 0x15, 0x04, 0, 0}}, {'+', {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0, 0}},
};

int ink(const uint8_t bits[8]) {
  int n = 0;
  for (int i = 0; i < 8; i++)
    n += __builtin_popcount(bits[i]);
  return n;
}

int distance(const uint8_t *a, const uint8_t *b) {
  int n = 0;
  for (int i = 0; i < 8; i++)
    n += __builtin_popcount(a[i] ^ b[i]);
  return n;
}

}  // namespace

void DotMatrix::compose(TextScreen &t) {
  /* Each cell's dots, and whether it is dim (only dim dots in it). */
  const bool depth = t.dimmable();
  uint8_t bits_of[TextScreen::MAX_ROWS][TextScreen::MAX_COLS][8];
  bool dim[TextScreen::MAX_ROWS][TextScreen::MAX_COLS];
  for (int r = 0; r < rows_; r++)
    for (int c = 0; c < cols_; c++) {
      bool any_bright = false;
      for (int y = 0; y < 8; y++) {
        bits_of[r][c][y] = depth ? bright_[r][c][y] | dim_[r][c][y] : cells_[r][c][y];
        any_bright |= bright_[r][c][y] != 0;
      }
      dim[r][c] = depth && !any_bright;
    }
  /* The lit cells, most ink first. */
  int order[TextScreen::MAX_ROWS * TextScreen::MAX_COLS];
  int weight[TextScreen::MAX_ROWS * TextScreen::MAX_COLS];
  int lit = 0;
  for (int r = 0; r < rows_; r++) {
    for (int c = 0; c < cols_; c++) {
      int w = ink(bits_of[r][c]);
      if (w == 0) {
        t.put(c, r, ' ');
        continue;
      }
      int j = lit++;
      while (j > 0 && weight[j - 1] < w) {
        order[j] = order[j - 1];
        weight[j] = weight[j - 1];
        j--;
      }
      order[j] = r * cols_ + c;
      weight[j] = w;
    }
  }
  for (int i = 0; i < lit; i++) {
    int r = order[i] / cols_, c = order[i] % cols_;
    const uint8_t *bits = bits_of[r][c];
    if (i < TextScreen::GLYPHS) {
      t.glyph(i, bits);
      t.put(c, r, i, dim[r][c]);
      continue;
    }
    const Stand *best = &STANDS[0];
    int bd = 99;
    for (const Stand &s : STANDS) {
      int d = distance(s.bits, bits);
      if (d < bd) {
        bd = d;
        best = &s;
      }
    }
    t.put(c, r, best->code, dim[r][c]);
  }
}

}  // namespace adcore
