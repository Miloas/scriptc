/* Adapted from QuickJS-NG quickjs.c, commit
 * 2f0aa72a6b09cf69ff06399bcf9f4c083ac1a278:
 * string_get, string_cmp, string_indexof_char, string_indexof.
 * Copyright (c) 2017-2026 Fabrice Bellard
 * Copyright (c) 2017-2024 Charlie Gordon
 * Copyright (c) 2023-2026 Ben Noordhuis
 * Copyright (c) 2023-2026 Saúl Ibarra Corretgé
 * MIT licensed; see LICENSE in this directory.
 *
 * The adapter supplies a resolved slice/flat span. Indices are UTF-16 code
 * units; 8-bit storage represents Latin-1, never encoded UTF-8. size_t and
 * subtraction-based bounds replace upstream's signed int length arithmetic.
 */
#ifndef SCR_QJS_STRING_H
#define SCR_QJS_STRING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const void *data;
  size_t len;
  bool wide;
} ScrQjsSpan;

static inline uint16_t scr_qjs_get(ScrQjsSpan s, size_t index) {
  return s.wide ? ((const uint16_t *)s.data)[index] : ((const uint8_t *)s.data)[index];
}

static int scr_qjs_cmp(ScrQjsSpan a, ScrQjsSpan b, size_t x, size_t y, size_t len) {
  for (size_t i = 0; i < len; i++) {
    int c1 = scr_qjs_get(a, x + i), c2 = scr_qjs_get(b, y + i);
    if (c1 != c2) return c1 - c2;
  }
  return 0;
}

static size_t scr_qjs_indexof_char(ScrQjsSpan s, uint16_t c, size_t from) {
  if (s.wide) {
    for (size_t i = from; i < s.len; i++)
      if (((const uint16_t *)s.data)[i] == c) return i;
  } else if (c <= 0xff) {
    for (size_t i = from; i < s.len; i++)
      if (((const uint8_t *)s.data)[i] == c) return i;
  }
  return SIZE_MAX;
}

static size_t scr_qjs_indexof(ScrQjsSpan s, ScrQjsSpan needle, size_t from) {
  if (needle.len == 0) return from;
  if (needle.len > s.len) return SIZE_MAX;
  uint16_t first = scr_qjs_get(needle, 0);
  size_t last = s.len - needle.len;
  for (size_t i = from; i <= last;) {
    size_t j = scr_qjs_indexof_char(s, first, i);
    if (j == SIZE_MAX || j > last) break;
    if (!scr_qjs_cmp(s, needle, j + 1, 1, needle.len - 1)) return j;
    i = j + 1;
  }
  return SIZE_MAX;
}
#endif
