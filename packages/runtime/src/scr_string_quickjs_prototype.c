/* PROTOTYPE ONLY. Included at the end of scr_string.c so decoding,
 * canonical WTF-8 encoding and allocation follow the real runtime. */
/* Source-enumerating builds may also compile this file independently.
 * Only scr_string.c supplies the private implementation context. */
#ifdef SCR_PROTO_STRING_IMPL
#include "../vendor/quickjs-strings/string.h"
#ifndef SCR_PROTO_SHARED
#define SCR_PROTO_SHARED 1
#endif
#ifndef SCR_PROTO_NATIVE_JOIN
#define SCR_PROTO_NATIVE_JOIN 1
#endif
#ifndef SCR_PROTO_VECTOR_UTF8
#define SCR_PROTO_VECTOR_UTF8 1
#endif
#ifndef SCR_PROTO_MIXED_UTF8
#define SCR_PROTO_MIXED_UTF8 1
#endif
#ifndef SCR_PROTO_DIRECT_DECODE
#define SCR_PROTO_DIRECT_DECODE 1
#endif
#ifndef SCR_PROTO_DECODE_SCAN
#define SCR_PROTO_DECODE_SCAN 1
#endif
#ifndef SCR_PROTO_DECODE_BULK
#define SCR_PROTO_DECODE_BULK 1
#endif
#ifndef SCR_PROTO_REPEAT_ONE
#define SCR_PROTO_REPEAT_ONE 0
#endif
#if SCR_PROTO_VECTOR_UTF8 && defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SCR_PROTO_NEON_UTF8 1
#include <arm_neon.h>
#include "scr_string_quickjs_utf8_table.h"
#else
#define SCR_PROTO_NEON_UTF8 0
#endif

typedef struct {
  size_t rc, length, marker;
  ScrStr *owner;
  const void *units;
  ScrStr *flat;
  bool wide, ascii, json_short;
} ScrProtoString;
_Static_assert(offsetof(ScrProtoString, owner) == sizeof(ScrStr), "prototype header ABI");
static SCR_TL size_t scr_proto_live, scr_proto_live_bytes, scr_proto_peak_bytes;
static SCR_TL size_t scr_proto_roots, scr_proto_views, scr_proto_materialized_bytes, scr_proto_json_bytes;
static SCR_TL size_t scr_proto_vector_units;
static SCR_TL size_t scr_proto_mixed_units;
static SCR_TL size_t scr_proto_decode_scratch_bytes, scr_proto_direct_decode_units;
static SCR_TL size_t scr_proto_builder_capacity_bytes, scr_proto_builder_length_bytes;
static SCR_TL size_t scr_proto_decode_layout_bytes;
static SCR_TL size_t scr_proto_bulk_decode_units;
static SCR_TL size_t scr_proto_repeat_one_reuses;

static ScrProtoString *scr_proto_alloc(size_t n, bool wide, ScrProtoString *owner, const void *units) {
  if (n > SCR_STR_MAX_CAP / 3) scr_oom();
  size_t size = sizeof(ScrProtoString) + (owner ? 0 : n << wide);
  ScrProtoString *q = scr_mem_alloc(size);
  if (!q) scr_oom();
  q->rc = 1; q->length = n; q->marker = SCR_STR_MAX_CAP;
  q->owner = owner ? scr_str_retain((ScrStr *)owner) : NULL;
  q->units = owner ? units : q + 1;
  q->flat = NULL;
  q->wide = wide; q->ascii = false; q->json_short = false;
  scr_proto_live++;
  scr_proto_live_bytes += size;
  if (scr_proto_live_bytes > scr_proto_peak_bytes) scr_proto_peak_bytes = scr_proto_live_bytes;
  if (owner) scr_proto_views++; else scr_proto_roots++;
#ifdef SCR_RC_AUDIT
  scr_live_strings++;
#endif
  return q;
}

static void scr_proto_release(ScrStr *s) {
  ScrProtoString *q = (ScrProtoString *)s;
  if (q->rc == SIZE_MAX) return;
  if (--q->rc) return;
  size_t bytes = sizeof(*q) + (q->owner ? 0 : q->length << q->wide);
  /* Legacy byte navigation can cache this address after borrowing its view.
   * Evict before malloc can recycle it. Do not encode a never-used view just
   * to destroy the string: only strings with bytes can enter those caches. */
  if (q->flat) scr_short_forget(s);
  scr_sidx_purge(s);
  scr_str_release(q->owner);
  scr_str_release(q->flat);
  scr_proto_live--; scr_proto_live_bytes -= bytes;
#ifdef SCR_RC_AUDIT
  scr_live_strings--;
#endif
  scr_mem_free(q);
}

static uint16_t scr_proto_unit(const ScrProtoString *s, size_t i) {
  return scr_qjs_get((ScrQjsSpan){s->units, s->length, s->wide}, i);
}

static ScrQjsSpan scr_proto_span(const ScrProtoString *s) {
  return (ScrQjsSpan){s->units, s->length, s->wide};
}

#if SCR_PROTO_COMPACT_JSON
/* These code points need at most three JSON bytes per UTF-16 unit.
 * Supplementary scalars need four bytes for their pair of units. */
static bool scr_proto_short_json_cp(uint32_t cp) {
  return (cp < 0xd800 || cp > 0xdfff) &&
      (cp >= 0x20 || (cp >= 8 && cp <= 13 && cp != 11));
}

/* Scan canonical UTF-8/WTF-8 separately from decoding so metadata does
 * not add work to every scalar decode. Latin-1 cannot contain ED leads. */
static bool scr_proto_short_json_utf8(const char *data, size_t n, bool latin1) {
  size_t i = 0;
#if SCR_PROTO_NEON_UTF8
  for (; n - i >= 17; i += 16) {
    uint8x16_t bytes = vld1q_u8((const uint8_t *)data + i);
    if (!latin1) {
      uint8x16_t ed = vceqq_u8(bytes, vdupq_n_u8(0xed));
      if (vmaxvq_u8(ed)) {
        uint8x16_t next = vld1q_u8((const uint8_t *)data + i + 1);
        if (vmaxvq_u8(vandq_u8(ed, vcgeq_u8(next, vdupq_n_u8(0xa0))))) return false;
      }
    }
    if (vminvq_u8(bytes) >= 0x20) continue;
    uint8x16_t short_control = vandq_u8(vcgeq_u8(bytes, vdupq_n_u8(8)),
        vcleq_u8(bytes, vdupq_n_u8(13)));
    short_control = vbicq_u8(short_control, vceqq_u8(bytes, vdupq_n_u8(11)));
    if (vmaxvq_u8(vbicq_u8(vcltq_u8(bytes, vdupq_n_u8(0x20)), short_control))) return false;
  }
#endif
  for (; i < n; i++) {
    if (!scr_proto_short_json_cp((uint8_t)data[i])) return false;
    if (!latin1 && (uint8_t)data[i] == 0xed && i + 1 < n &&
        (uint8_t)data[i + 1] >= 0xa0) return false;
  }
  return true;
}

static bool scr_proto_short_json_string(const ScrStr *s) {
  if (!s || !s->len) return true;
  if (scr_proto_is(s)) return ((const ScrProtoString *)s)->json_short;
  return s->ascii && scr_proto_short_json_utf8(s->data, s->len, true);
}
#endif

#if SCR_PROTO_DECODE_SCAN
typedef struct { size_t length; bool wide, has_ascii, uniform_wide; } ScrProtoLayout;

/* Count canonical UTF-8/WTF-8 units directly. Navigation checkpoints
 * have no use once the string becomes native, indexed code units. */
static ScrProtoLayout scr_proto_decode_layout(ScrStr *s) {
  ScrProtoLayout layout = {s->len, false, s->ascii, false};
  if (s->ascii) return layout;
  size_t i = 0, continuation = 0, supplementary = 0;
#if SCR_PROTO_NEON_UTF8
  uint8x16_t maximum = vdupq_n_u8(0);
  uint8x16_t minimum = vdupq_n_u8(255);
  for (; s->len - i >= 16; i += 16) {
    uint8x16_t bytes = vld1q_u8((const uint8_t *)s->data + i);
    uint8x16_t cont = vceqq_u8(vandq_u8(bytes, vdupq_n_u8(0xc0)), vdupq_n_u8(0x80));
    uint8x16_t four = vcgeq_u8(bytes, vdupq_n_u8(0xf0));
    continuation += vaddvq_u8(vshrq_n_u8(cont, 7));
    supplementary += vaddvq_u8(vshrq_n_u8(four, 7));
    maximum = vmaxq_u8(maximum, bytes);
    minimum = vminq_u8(minimum, bytes);
  }
  layout.wide = vmaxvq_u8(maximum) >= 0xc4;
  layout.has_ascii = vminvq_u8(minimum) < 0x80;
#endif
  for (; i < s->len; i++) {
    uint8_t byte = (uint8_t)s->data[i];
    continuation += (byte & 0xc0) == 0x80;
    supplementary += byte >= 0xf0;
    layout.wide |= byte >= 0xc4;
    layout.has_ascii |= byte < 0x80;
  }
  layout.length = s->len - continuation + supplementary;
  layout.uniform_wide = (s->len % 3 == 0 && layout.length == s->len / 3) ||
      (supplementary != 0 && layout.length == supplementary * 2);
  scr_str_length_cache_set(s, (uint32_t)layout.length);
  scr_proto_decode_layout_bytes += s->len;
  return layout;
}
#endif

#if SCR_PROTO_DECODE_BULK && SCR_PROTO_NEON_UTF8
typedef struct { size_t bytes, units; } ScrProtoDecoded;

/* The source is already canonical UTF-8/WTF-8. Dispatch only complete
 * homogeneous groups; mixed groups fall back to the scalar decoder. */
static inline __attribute__((always_inline)) ScrProtoDecoded scr_proto_decode_block(
    const uint8_t *src, size_t n, void *dst, bool wide) {
  uint8_t first = src[0];
  uint32_t cp;
  if (first < 0x80) {
    if (n >= 8) {
      uint8x8_t bytes = vld1_u8(src);
      if (vmaxv_u8(bytes) < 0x80) {
        if (wide) vst1q_u16(dst, vmovl_u8(bytes));
        else vst1_u8(dst, bytes);
        return (ScrProtoDecoded){8, 8};
      }
    }
    if (wide) *(uint16_t *)dst = first;
    else *(uint8_t *)dst = first;
    return (ScrProtoDecoded){1, 1};
  }
  if (first < 0xe0) {
    if (n < 16 || (src[2] & 0xe0) != 0xc0 || (src[14] & 0xe0) != 0xc0)
      goto scalar_two;
    uint8x8x2_t bytes = vld2_u8(src);
    if (vmaxv_u8(veor_u8(vand_u8(bytes.val[0], vdup_n_u8(0xe0)), vdup_n_u8(0xc0))))
      goto scalar_two;
    uint16x8_t units = vorrq_u16(vshll_n_u8(vand_u8(bytes.val[0], vdup_n_u8(31)), 6),
        vmovl_u8(vand_u8(bytes.val[1], vdup_n_u8(63))));
    if (wide) vst1q_u16(dst, units);
    else vst1_u8(dst, vmovn_u16(units));
    return (ScrProtoDecoded){16, 8};
scalar_two:
    cp = ((uint32_t)(first & 31) << 6) | (src[1] & 63);
    if (wide) *(uint16_t *)dst = (uint16_t)cp;
    else *(uint8_t *)dst = (uint8_t)cp;
    return (ScrProtoDecoded){2, 1};
  }
  if (first < 0xf0) {
    if (n < 24 || (src[3] & 0xf0) != 0xe0 || (src[6] & 0xf0) != 0xe0)
      goto scalar_three;
    uint8x8x3_t bytes = vld3_u8(src);
    if (vmaxv_u8(veor_u8(vand_u8(bytes.val[0], vdup_n_u8(0xf0)), vdup_n_u8(0xe0))))
      goto scalar_three;
    uint16x8_t units = vorrq_u16(vshlq_n_u16(vmovl_u8(vand_u8(bytes.val[0], vdup_n_u8(15))), 12),
        vorrq_u16(vshll_n_u8(vand_u8(bytes.val[1], vdup_n_u8(63)), 6),
        vmovl_u8(vand_u8(bytes.val[2], vdup_n_u8(63)))));
    vst1q_u16(dst, units);
    return (ScrProtoDecoded){24, 8};
scalar_three:
    *(uint16_t *)dst = (uint16_t)(((uint32_t)(first & 15) << 12) | ((uint32_t)(src[1] & 63) << 6) | (src[2] & 63));
    return (ScrProtoDecoded){3, 1};
  }
  if (n < 16 || (src[4] & 0xf8) != 0xf0) goto scalar_four;
  uint32x4_t bytes = vreinterpretq_u32_u8(vld1q_u8(src));
  if (vmaxvq_u32(veorq_u32(vandq_u32(bytes, vdupq_n_u32(0xf8)), vdupq_n_u32(0xf0))))
    goto scalar_four;
  uint32x4_t scalars = vorrq_u32(vshlq_n_u32(vandq_u32(bytes, vdupq_n_u32(7)), 18),
      vorrq_u32(vshlq_n_u32(vandq_u32(bytes, vdupq_n_u32(0x3f00)), 4),
      vorrq_u32(vshrq_n_u32(vandq_u32(bytes, vdupq_n_u32(0x3f0000)), 10),
          vshrq_n_u32(vandq_u32(bytes, vdupq_n_u32(0x3f000000)), 24))));
  uint16x4_t high = vorr_u16(vmovn_u32(vshrq_n_u32(vsubq_u32(scalars, vdupq_n_u32(0x10000)), 10)), vdup_n_u16(0xd800));
  uint16x4_t low = vorr_u16(vmovn_u32(vandq_u32(scalars, vdupq_n_u32(1023))), vdup_n_u16(0xdc00));
  uint16x4x2_t pairs = vzip_u16(high, low);
  vst1q_u16(dst, vcombine_u16(pairs.val[0], pairs.val[1]));
  return (ScrProtoDecoded){16, 8};
scalar_four:
  cp = ((uint32_t)(first & 7) << 18) | ((uint32_t)(src[1] & 63) << 12) | ((uint32_t)(src[2] & 63) << 6) | (src[3] & 63);
  ((uint16_t *)dst)[0] = (uint16_t)scr_cp_high(cp);
  ((uint16_t *)dst)[1] = (uint16_t)scr_cp_low(cp);
  return (ScrProtoDecoded){4, 2};
}
#endif

static ScrProtoString *scr_proto_from_flat(ScrStr *s) {
  if (scr_proto_is(s)) return (ScrProtoString *)scr_str_retain(s);
#if SCR_PROTO_DECODE_SCAN
  ScrProtoLayout layout = scr_proto_decode_layout(s);
  size_t n = layout.length;
#else
  size_t n = (size_t)scr_str_utf16_len(s);
#endif
#if SCR_PROTO_DIRECT_DECODE
  /* Equal UTF-8 byte and UTF-16 unit lengths prove ASCII, including
   * builder strings whose ASCII header bit has not been populated. */
  if (n == s->len) {
    ScrProtoString *q = scr_proto_alloc(n, false, NULL, NULL);
    q->ascii = true;
#if SCR_PROTO_COMPACT_JSON
    q->json_short = scr_proto_short_json_utf8(s->data, n, true);
#endif
    memcpy((void *)q->units, s->data, n);
    scr_proto_direct_decode_units += n;
    return q;
  }
  /* In canonical UTF-8/WTF-8, a byte >= C4 is a leading byte for a
   * value outside Latin-1. Continuation bytes cannot satisfy this. */
#if SCR_PROTO_DECODE_SCAN
  bool wide = layout.wide;
#else
  bool wide = false;
  for (size_t i = 0; i < s->len; i++) {
    if ((uint8_t)s->data[i] >= 0xc4) { wide = true; break; }
  }
#endif
  ScrProtoString *q = scr_proto_alloc(n, wide, NULL, NULL);
  size_t at = 0, bulk_units = 0;
  if (wide) {
    uint16_t *out = (uint16_t *)q->units;
#if SCR_PROTO_DECODE_BULK && SCR_PROTO_NEON_UTF8
    /* Without ASCII or a uniform wide width, bulk probes repeatedly
     * miss on interleaved BMP/supplementary text. Use the scalar loop. */
#if SCR_PROTO_DECODE_SCAN
    bool bulk_wide = layout.has_ascii || layout.uniform_wide;
#else
    bool bulk_wide = true;
#endif
    if (bulk_wide) {
      for (size_t i = 0; i < s->len;) {
        ScrProtoDecoded block = scr_proto_decode_block((const uint8_t *)s->data + i, s->len - i, out + at, true);
        i += block.bytes; at += block.units;
        if (block.units == 8) bulk_units += 8;
      }
    } else
#endif
    {
      for (size_t i = 0; i < s->len;) {
        size_t adv;
        uint32_t cp = scr_utf8_decode(s->data + i, &adv);
        i += adv;
        if (cp > 0xffff) {
          out[at++] = (uint16_t)scr_cp_high(cp);
          out[at++] = (uint16_t)scr_cp_low(cp);
        } else out[at++] = (uint16_t)cp;
      }
    }
  } else {
    uint8_t *out = (uint8_t *)q->units;
    for (size_t i = 0; i < s->len;) {
#if SCR_PROTO_DECODE_BULK && SCR_PROTO_NEON_UTF8
      ScrProtoDecoded block = scr_proto_decode_block((const uint8_t *)s->data + i, s->len - i, out + at, false);
      i += block.bytes; at += block.units;
      if (block.units == 8) bulk_units += 8;
#else
      uint8_t cp = (uint8_t)s->data[i++];
      if (cp >= 0x80) cp = (uint8_t)(((cp & 31) << 6) | ((uint8_t)s->data[i++] & 63));
      out[at++] = cp;
#endif
    }
  }
  if (at != n) scr_trap("prototype direct decode length mismatch");
#if SCR_PROTO_COMPACT_JSON
  q->json_short = scr_proto_short_json_utf8(s->data, s->len, !wide);
#endif
  scr_proto_direct_decode_units += n;
  scr_proto_bulk_decode_units += bulk_units;
  return q;
#else
  uint16_t *units = malloc((n ? n : 1) * sizeof(*units));
  if (!units) scr_oom();
  scr_proto_decode_scratch_bytes += (n ? n : 1) * sizeof(*units);
  size_t at = 0;
  bool wide = false, ascii = true;
  for (size_t i = 0; i < s->len;) {
    size_t adv;
    uint32_t cp = scr_utf8_decode(s->data + i, &adv);
    i += adv; wide |= cp > 255; ascii &= cp < 128;
    if (cp > 0xffff) {
      units[at++] = (uint16_t)scr_cp_high(cp);
      units[at++] = (uint16_t)scr_cp_low(cp);
    } else units[at++] = (uint16_t)cp;
  }
  if (at != n) scr_trap("prototype decode length mismatch");
  ScrProtoString *q = scr_proto_alloc(n, wide, NULL, NULL);
  q->ascii = ascii;
#if SCR_PROTO_COMPACT_JSON
  q->json_short = scr_proto_short_json_utf8(s->data, s->len, !wide);
#endif
  if (wide) memcpy((void *)q->units, units, n * sizeof(*units));
  else for (size_t i = 0; i < n; i++) ((uint8_t *)q->units)[i] = (uint8_t)units[i];
  free(units);
  return q;
#endif
}

const char *scr_proto_ascii_data(const ScrStr *s, size_t *length) {
  const ScrProtoString *q = (const ScrProtoString *)s;
  if (!q->ascii || q->wide) return NULL;
  *length = q->length;
  return q->units;
}

void scr_proto_note_json(size_t bytes) { scr_proto_json_bytes += bytes; }

/* A proven short-escape string needs <= 3 bytes per unit. Sixteen extra
 * bytes cover the SIMD stores' padding, in addition to the two quotes. */
size_t scr_proto_json_capacity(const ScrStr *s) {
  bool short_json = ((const ScrProtoString *)s)->json_short;
  size_t scale = short_json ? 3 : 6, padding = short_json ? 18 : 2;
  if (s->len > (SIZE_MAX - padding) / scale) scr_oom();
  return s->len * scale + padding;
}

void scr_proto_note_builder_capacity(size_t capacity, size_t length) {
  scr_proto_builder_capacity_bytes += capacity;
  scr_proto_builder_length_bytes += length;
}

#if SCR_PROTO_NEON_UTF8
/* Eight readable units and room for padded stores are guaranteed by
 * the caller's JSON reservation. Each group stores 16 bytes, including
 * padding overwritten by the next group or by subsequent output. */
static inline __attribute__((always_inline)) bool scr_proto_json_block(
    const uint16_t *src, char *dst, size_t *written) {
  uint16x8_t units = vld1q_u16(src);
  if (vminvq_u16(units) < 0x800) {
    if (vmaxvq_u16(units) >= 0x80) return false;
    uint16x8_t special = vorrq_u16(vcltq_u16(units, vdupq_n_u16(0x20)),
      vorrq_u16(vceqq_u16(units, vdupq_n_u16('"')), vceqq_u16(units, vdupq_n_u16('\\'))));
    if (vmaxvq_u16(special)) return false;
    vst1_u8((uint8_t *)dst, vmovn_u16(units));
    *written = 8;
    return true;
  }
  uint16x8_t kind = vandq_u16(units, vdupq_n_u16(0xfc00));
  uint16x8_t high = vceqq_u16(kind, vdupq_n_u16(0xd800));
  uint16x8_t low = vceqq_u16(kind, vdupq_n_u16(0xdc00));
  if (!vmaxvq_u16(vorrq_u16(high, low))) {
    uint8x8x3_t bytes = {{
      vmovn_u16(vorrq_u16(vshrq_n_u16(units, 12), vdupq_n_u16(0xe0))),
      vmovn_u16(vorrq_u16(vandq_u16(vshrq_n_u16(units, 6), vdupq_n_u16(63)), vdupq_n_u16(0x80))),
      vmovn_u16(vorrq_u16(vandq_u16(units, vdupq_n_u16(63)), vdupq_n_u16(0x80)))
    }};
    vst3_u8((uint8_t *)dst, bytes);
    *written = 24;
    return true;
  }
  /* A pair crossing either end, or a lone surrogate anywhere, falls back
   * to the scalar encoder, which owns the full JSON/WTF-8 semantics. */
  if (vgetq_lane_u16(low, 0) || vgetq_lane_u16(high, 7) ||
      vmaxvq_u16(veorq_u16(high, vextq_u16(low, low, 1)))) return false;
  uint16x8_t next = vextq_u16(units, units, 1);
  uint16x8_t tags = vorrq_u16(vandq_u16(high, vdupq_n_u16(1)), vandq_u16(low, vdupq_n_u16(2)));
  const uint16x4_t weights = {1, 3, 9, 27};
  unsigned first = vaddv_u16(vmul_u16(vget_low_u16(tags), weights));
  unsigned second = vaddv_u16(vmul_u16(vget_high_u16(tags), weights));
  /* Form two UTF-8 bytes in each 16-bit lane, then interleave the halves.
   * For a pair, cp = ((high & 1023) + 64) * 1024 + (low & 1023). */
  uint16x8_t adjusted = vaddq_u16(vandq_u16(units, vdupq_n_u16(1023)), vdupq_n_u16(64));
  uint16x8_t bmp_prefix = vorrq_u16(vorrq_u16(vshrq_n_u16(units, 12),
      vandq_u16(vshlq_n_u16(units, 2), vdupq_n_u16(0x3f00))), vdupq_n_u16(0x80e0));
  uint16x8_t pair_prefix = vorrq_u16(vorrq_u16(vshrq_n_u16(adjusted, 8),
      vandq_u16(vshlq_n_u16(adjusted, 6), vdupq_n_u16(0x3f00))), vdupq_n_u16(0x80f0));
  uint16x8_t bmp_tail = vorrq_u16(vandq_u16(units, vdupq_n_u16(63)), vdupq_n_u16(0x80));
  uint16x8_t pair_tail = vorrq_u16(vorrq_u16(
      vandq_u16(vshlq_n_u16(adjusted, 4), vdupq_n_u16(0x30)),
      vandq_u16(vshrq_n_u16(next, 6), vdupq_n_u16(15))),
      vorrq_u16(vandq_u16(vshlq_n_u16(next, 8), vdupq_n_u16(0x3f00)), vdupq_n_u16(0x8080)));
  uint16x8_t prefix = vbslq_u16(high, pair_prefix, bmp_prefix);
  uint16x8_t tail = vbslq_u16(high, pair_tail, bmp_tail);
  uint8x16_t a = vreinterpretq_u8_u16(vzip1q_u16(prefix, tail));
  uint8x16_t b = vreinterpretq_u8_u16(vzip2q_u16(prefix, tail));
  const ScrProtoUtf8Shuffle *sa = &scr_proto_utf8_shuffle[first], *sb = &scr_proto_utf8_shuffle[second];
  vst1q_u8((uint8_t *)dst, vqtbl1q_u8(a, vld1q_u8(sa->index)));
  vst1q_u8((uint8_t *)dst + sa->length, vqtbl1q_u8(b, vld1q_u8(sb->index)));
  *written = (size_t)sa->length + sb->length;
  return true;
}

#if SCR_PROTO_MIXED_UTF8
static inline __attribute__((always_inline)) size_t scr_proto_json_pack_mixed(
    uint16x8_t prefix, uint16x8_t tail, uint16x8_t tags, char *dst) {
  const uint16x4_t weights = {1, 3, 9, 27};
  unsigned first = vaddv_u16(vmul_u16(vget_low_u16(tags), weights));
  unsigned second = vaddv_u16(vmul_u16(vget_high_u16(tags), weights));
  uint8x16_t a = vreinterpretq_u8_u16(vzip1q_u16(prefix, tail));
  uint8x16_t b = vreinterpretq_u8_u16(vzip2q_u16(prefix, tail));
  const ScrProtoUtf8Shuffle *sa = &scr_proto_utf8_mixed_shuffle[first], *sb = &scr_proto_utf8_mixed_shuffle[second];
  vst1q_u8((uint8_t *)dst, vqtbl1q_u8(a, vld1q_u8(sa->index)));
  vst1q_u8((uint8_t *)dst + sa->length, vqtbl1q_u8(b, vld1q_u8(sb->index)));
  return (size_t)sa->length + sb->length;
}

/* Each lane emits one to three bytes. A pair emits two in the high
 * lane and two in the low lane, keeping the shuffle table small. A
 * trailing high is deferred; its dummy output byte is not counted. */
static inline __attribute__((always_inline)) bool scr_proto_json_mixed_block(
    const uint16_t *src, char *dst, size_t *written, size_t *consumed) {
  uint16x8_t units = vld1q_u16(src);
  uint16x8_t control = vcltq_u16(units, vdupq_n_u16(0x20));
  uint16x8_t special = vorrq_u16(control,
      vorrq_u16(vceqq_u16(units, vdupq_n_u16('"')), vceqq_u16(units, vdupq_n_u16('\\'))));
  uint16x8_t escape = units;
  if (vmaxvq_u16(control)) {
    const uint8x16x2_t names = {{
      {0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
    }};
    uint8x16_t lookup = vqtbl2q_u8(names, vcombine_u8(vmovn_u16(units), vdup_n_u8(255)));
    uint16x8_t names16 = vmovl_u8(vget_low_u8(lookup));
    if (vmaxvq_u16(vandq_u16(control, vceqq_u16(names16, vdupq_n_u16(0))))) return false;
    escape = vbslq_u16(control, names16, units);
  }
  uint16x8_t two = vcgeq_u16(units, vdupq_n_u16(0x80));
  uint16x8_t prefix2 = vorrq_u16(vorrq_u16(vshrq_n_u16(units, 6),
      vandq_u16(vshlq_n_u16(units, 8), vdupq_n_u16(0x3f00))), vdupq_n_u16(0x80c0));
  uint16x8_t escaped = vorrq_u16(vshlq_n_u16(escape, 8), vdupq_n_u16('\\'));
  if (vmaxvq_u16(units) < 0x800) {
    uint16x8_t prefix = vbslq_u16(special, escaped, vbslq_u16(two, prefix2, units));
    uint16x8_t tags = vandq_u16(vorrq_u16(two, special), vdupq_n_u16(1));
    *written = scr_proto_json_pack_mixed(prefix, vdupq_n_u16(0), tags, dst);
    *consumed = 8;
    return true;
  }
  uint16x8_t kind = vandq_u16(units, vdupq_n_u16(0xfc00));
  uint16x8_t high = vceqq_u16(kind, vdupq_n_u16(0xd800));
  uint16x8_t low = vceqq_u16(kind, vdupq_n_u16(0xdc00));
  if (vgetq_lane_u16(low, 0) ||
      vmaxvq_u16(veorq_u16(vsetq_lane_u16(0, high, 7), vextq_u16(low, low, 1)))) return false;
  *consumed = vgetq_lane_u16(high, 7) ? 7 : 8;

  uint16x8_t three = vcgeq_u16(units, vdupq_n_u16(0x800));
  uint16x8_t prefix3 = vorrq_u16(vorrq_u16(vshrq_n_u16(units, 12),
      vandq_u16(vshlq_n_u16(units, 2), vdupq_n_u16(0x3f00))), vdupq_n_u16(0x80e0));
  uint16x8_t prefix = vbslq_u16(three, prefix3, vbslq_u16(two, prefix2, units));
  uint16x8_t tail = vorrq_u16(vandq_u16(units, vdupq_n_u16(63)), vdupq_n_u16(0x80));
  if (vmaxvq_u16(high)) {
    uint16x8_t adjusted = vaddq_u16(vandq_u16(units, vdupq_n_u16(1023)), vdupq_n_u16(64));
    uint16x8_t prefix4 = vorrq_u16(vorrq_u16(vshrq_n_u16(adjusted, 8),
        vandq_u16(vshlq_n_u16(adjusted, 6), vdupq_n_u16(0x3f00))), vdupq_n_u16(0x80f0));
    uint16x8_t previous = vextq_u16(adjusted, adjusted, 7);
    uint16x8_t tail4 = vorrq_u16(vorrq_u16(
        vandq_u16(vshlq_n_u16(previous, 4), vdupq_n_u16(0x30)),
        vandq_u16(vshrq_n_u16(units, 6), vdupq_n_u16(15))),
        vorrq_u16(vandq_u16(vshlq_n_u16(units, 8), vdupq_n_u16(0x3f00)), vdupq_n_u16(0x8080)));
    prefix = vbslq_u16(high, prefix4, prefix);
    prefix = vbslq_u16(low, tail4, prefix);
  }
  prefix = vbslq_u16(special, escaped, prefix);
  uint16x8_t tags = vaddq_u16(vandq_u16(two, vdupq_n_u16(1)), vandq_u16(three, vdupq_n_u16(1)));
  tags = vbslq_u16(vorrq_u16(special, vorrq_u16(high, low)), vdupq_n_u16(1), tags);
  if (*consumed == 7) tags = vsetq_lane_u16(0, tags, 7);
  *written = scr_proto_json_pack_mixed(prefix, tail, tags, dst) - (*consumed == 7);
  return true;
}
#endif
#endif

/* Constant-width/constant-mode dispatch lets the compiler drop UTF-16
 * and JSON branches from the Latin-1 and raw-output loops. */
static inline __attribute__((always_inline)) size_t scr_proto_encode_units(
    const void *src, size_t length, char *restrict dst, bool json, bool wide, bool quoted) {
  const uint8_t *bytes = src;
  const uint16_t *units = src;
  static const char hex[] = "0123456789abcdef";
  size_t used = 0;
  if (quoted) dst[used++] = '"';
  for (size_t i = 0; i < length; i++) {
    uint32_t cp = wide ? units[i] : bytes[i];
    if (cp < 0x80) {
      if (!json || (cp >= 0x20 && cp != '"' && cp != '\\')) {
        dst[used++] = (char)cp;
        continue;
      }
      dst[used++] = '\\';
      switch (cp) {
      case '"': case '\\': dst[used++] = (char)cp; break;
      case '\b': dst[used++] = 'b'; break;
      case '\f': dst[used++] = 'f'; break;
      case '\n': dst[used++] = 'n'; break;
      case '\r': dst[used++] = 'r'; break;
      case '\t': dst[used++] = 't'; break;
      default:
        dst[used++] = 'u'; dst[used++] = '0'; dst[used++] = '0';
        dst[used++] = hex[cp >> 4]; dst[used++] = hex[cp & 15];
      }
    } else if (cp < 0x800) {
      dst[used++] = (char)(0xc0 | (cp >> 6));
      dst[used++] = (char)(0x80 | (cp & 63));
    } else {
      if ((cp & 0xf800) == 0xd800) {
        if (cp < 0xdc00 && i + 1 < length) {
          uint32_t low = units[i + 1];
          if (low >= 0xdc00 && low <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            i++;
            dst[used++] = (char)(0xf0 | (cp >> 18));
            dst[used++] = (char)(0x80 | ((cp >> 12) & 63));
            dst[used++] = (char)(0x80 | ((cp >> 6) & 63));
            dst[used++] = (char)(0x80 | (cp & 63));
            continue;
          }
        }
        if (json) {
          dst[used++] = '\\'; dst[used++] = 'u';
          dst[used++] = hex[cp >> 12]; dst[used++] = hex[(cp >> 8) & 15];
          dst[used++] = hex[(cp >> 4) & 15]; dst[used++] = hex[cp & 15];
          continue;
        }
        scr_wtf8_note();
      }
      dst[used++] = (char)(0xe0 | (cp >> 12));
      dst[used++] = (char)(0x80 | ((cp >> 6) & 63));
      dst[used++] = (char)(0x80 | (cp & 63));
    }
  }
  if (quoted) dst[used++] = '"';
  return used;
}

#if SCR_PROTO_NEON_UTF8
/* Keep the scalar loop out of the SIMD function's register allocation. */
static __attribute__((noinline)) size_t scr_proto_json_scalar_run(
    const uint16_t *src, size_t length, char *dst) {
  return scr_proto_encode_units(src, length, dst, true, true, false);
}

typedef struct { size_t units, bytes; } ScrProtoEncodedRun;

/* Keep SIMD constants live through the whole run; scalar calls would
 * otherwise force the caller to reload them between blocks. */
static __attribute__((noinline)) ScrProtoEncodedRun scr_proto_json_vector_run(
    const uint16_t *src, size_t length, char *dst) {
  ScrProtoEncodedRun run = {0, 0};
  size_t bytes;
  while (length - run.units >= 8 &&
      scr_proto_json_block(src + run.units, dst + run.bytes, &bytes)) {
    run.units += 8;
    run.bytes += bytes;
  }
  return run;
}

#if SCR_PROTO_MIXED_UTF8
/* Consecutive pairs need no variable-length byte compaction. Keep this
 * loop separate so its constants do not burden the BMP/mixed loops. */
static __attribute__((noinline)) ScrProtoEncodedRun scr_proto_json_emoji_run(
    const uint16_t *src, size_t length, char *dst) {
  ScrProtoEncodedRun run = {0, 0};
  const uint16x8_t kinds = {0xd800, 0xdc00, 0xd800, 0xdc00, 0xd800, 0xdc00, 0xd800, 0xdc00};
  while (length - run.units >= 8) {
    uint16x8_t units = vld1q_u16(src + run.units);
    if (vmaxvq_u16(veorq_u16(vandq_u16(units, vdupq_n_u16(0xfc00)), kinds))) break;
    uint16x4_t high = vget_low_u16(vuzp1q_u16(units, units));
    uint16x4_t low = vget_low_u16(vuzp2q_u16(units, units));
    uint32x4_t cp = vaddq_u32(vshll_n_u16(vand_u16(high, vdup_n_u16(1023)), 10),
        vmovl_u16(vand_u16(low, vdup_n_u16(1023))));
    cp = vaddq_u32(cp, vdupq_n_u32(0x10000));
    uint32x4_t packed = vorrq_u32(vorrq_u32(vshrq_n_u32(cp, 18),
        vandq_u32(vshrq_n_u32(cp, 4), vdupq_n_u32(0x3f00))),
        vorrq_u32(vandq_u32(vshlq_n_u32(cp, 10), vdupq_n_u32(0x3f0000)),
        vandq_u32(vshlq_n_u32(cp, 24), vdupq_n_u32(0x3f000000))));
    packed = vorrq_u32(packed, vdupq_n_u32(0x808080f0));
    vst1q_u8((uint8_t *)dst + run.bytes, vreinterpretq_u8_u32(packed));
    run.units += 8; run.bytes += 16;
  }
  return run;
}

static __attribute__((noinline)) ScrProtoEncodedRun scr_proto_json_mixed_run(
    const uint16_t *src, size_t length, char *dst) {
  ScrProtoEncodedRun run = {0, 0};
  size_t bytes, units, next_probe = 32;
  while (length - run.units >= 8) {
    /* Resume the cheaper BMP/surrogate loop after an isolated escape.
     * Always try the first block: the pure loop may have rejected a
     * surrogate that crosses its block edge. */
    if (run.units >= next_probe) {
      if (vminvq_u16(vld1q_u16(src + run.units)) >= 0x800) break;
      next_probe = run.units + 32;
    }
    if (!scr_proto_json_mixed_block(src + run.units, dst + run.bytes, &bytes, &units)) break;
    run.units += units;
    run.bytes += bytes;
  }
  return run;
}
#endif

static size_t scr_proto_encode_wide_json(const uint16_t *src, size_t length, char *dst) {
  size_t at = 0, used = 1, vector_units = 0, scalar_window = 128;
  dst[0] = '"';
  while (at < length) {
#if SCR_PROTO_MIXED_UTF8
    if (length - at >= 8 && (src[at] & 0xfc00) == 0xd800) {
      ScrProtoEncodedRun emoji = scr_proto_json_emoji_run(src + at, length - at, dst + used);
      at += emoji.units; used += emoji.bytes; vector_units += emoji.units;
      if (at == length) break;
    }
#endif
    ScrProtoEncodedRun run = scr_proto_json_vector_run(src + at, length - at, dst + used);
    at += run.units; used += run.bytes; vector_units += run.units;
    if (at == length) break;
    if (run.units) scalar_window = 128;
#if SCR_PROTO_MIXED_UTF8
    run = scr_proto_json_mixed_run(src + at, length - at, dst + used);
    at += run.units; used += run.bytes; vector_units += run.units;
    scr_proto_mixed_units += run.units;
    if (at == length) break;
    if (run.units) { scalar_window = 128; continue; }
#endif
    /* Amortize a failed SIMD probe over a scalar run. Keep surrogate
     * pairs together when the run ends before the end of the string. */
    size_t count = length - at < scalar_window ? length - at : scalar_window;
    if (count < length - at && (src[at + count - 1] & 0xfc00) == 0xd800 &&
        (src[at + count] & 0xfc00) == 0xdc00) count++;
    used += scr_proto_json_scalar_run(src + at, count, dst + used);
    at += count;
    if (scalar_window < 1024) scalar_window *= 2;
  }
  dst[used++] = '"';
  scr_proto_vector_units += vector_units;
  return used;
}
#endif

size_t scr_proto_encode(const ScrStr *s, char *dst, bool json) {
  const ScrProtoString *q = (const ScrProtoString *)s;
  size_t used;
  if (json) {
    if (q->wide) {
#if SCR_PROTO_NEON_UTF8
      used = scr_proto_encode_wide_json(q->units, q->length, dst);
#else
      used = scr_proto_encode_units(q->units, q->length, dst, true, true, true);
#endif
    } else used = scr_proto_encode_units(q->units, q->length, dst, true, false, true);
    scr_proto_note_json(used);
  } else {
    used = q->wide ? scr_proto_encode_units(q->units, q->length, dst, false, true, false)
                   : scr_proto_encode_units(q->units, q->length, dst, false, false, false);
  }
  return used;
}

ScrStr *scr_proto_flat(const ScrStr *s) {
  if (!scr_proto_is(s)) return scr_str_retain((ScrStr *)s);
  const ScrProtoString *q = (const ScrProtoString *)s;
  ScrStr *flat = scr_str_alloc(q->length * 3, q->length * 3);
  size_t used = scr_proto_encode(s, flat->data, false);
  flat->len = used; flat->data[used] = 0; flat->ascii = used == q->length;
  scr_str_length_cache_set(flat, (uint32_t)q->length);
  scr_proto_materialized_bytes += used;
  return flat;
}

#if SCR_QUICKJS_STRINGS
ScrStr *scr_str_utf8_view(const ScrStr *s) {
  if (!scr_proto_is(s)) return (ScrStr *)s;
  ScrProtoString *q = (ScrProtoString *)s;
  if (!q->flat) q->flat = scr_proto_flat(s);
  return q->flat;
}

void scr_str_prepare_publish(ScrStr *s) {
  /* Compute any lazy bytes while this object still belongs to one thread.
   * The published native object retains its ordinary root and cached flat
   * allocation forever; readers neither mutate nor release those owners. */
  (void)scr_str_flat_view(s);
}

void scr_str_visit_owners(ScrStr *s, ScrStrOwnerVisitor visit, void *context) {
  if (!scr_proto_is(s)) return;
  ScrProtoString *q = (ScrProtoString *)s;
  if (q->owner) visit(q->owner, context);
  if (q->flat) visit(q->flat, context);
}
#endif

static double scr_proto_char_code_at(ScrStr *s, double index) {
  double n = scr_to_integer_or_infinity(index);
  if (n < 0 || n >= (double)s->len) return NAN;
  return (double)scr_proto_unit((ScrProtoString *)s, (size_t)n);
}

static ScrStr *scr_proto_repeat(ScrStr *s, double n) {
  ScrProtoString *source = scr_proto_from_flat(s);
#if SCR_PROTO_REPEAT_ONE
  if (n == 1) {
    scr_proto_repeat_one_reuses++;
    return (ScrStr *)source; /* from_flat already returns an owned reference */
  }
#endif
  if (n > (double)(SCR_STR_MAX_CAP / 3) / (double)source->length) scr_oom();
  size_t length = source->length * (size_t)n;
  ScrProtoString *q = scr_proto_alloc(length, source->wide, NULL, NULL);
  q->ascii = source->ascii;
  q->json_short = source->json_short;
  size_t initial = source->length << source->wide;
  size_t bytes = length << source->wide;
  memcpy((void *)q->units, source->units, initial);
  for (size_t done = initial; done < bytes;) {
    size_t count = done < bytes - done ? done : bytes - done;
    memcpy((char *)q->units + done, q->units, count); done += count;
  }
  scr_str_release((ScrStr *)source);
  return (ScrStr *)q;
}

static void scr_proto_range(ScrStr *s, double start, double end, bool substring, ScrStringSlice *out) {
  double a = scr_to_integer_or_infinity(start), b = scr_to_integer_or_infinity(end);
  size_t from, to;
  if (substring) {
    from = a <= 0 ? 0 : a >= (double)s->len ? s->len : (size_t)a;
    to = b <= 0 ? 0 : b >= (double)s->len ? s->len : (size_t)b;
    if (from > to) { size_t swap = from; from = to; to = swap; }
  } else {
    from = scr_slice_boundary(a, s->len); to = scr_slice_boundary(b, s->len);
  }
  out->start = from; out->length = to > from ? to - from : 0; out->split = 0;
}

static ScrStr *scr_proto_slice(ScrStr *s, double from, double to, bool substring) {
  ScrStringSlice range;
  scr_proto_range(s, from, to, substring, &range);
  if (range.length == 0) return scr_str_empty();
  if (range.start == 0 && range.length == s->len) return scr_str_retain(s);
  ScrProtoString *source = (ScrProtoString *)s;
  const void *units = (const char *)source->units + (range.start << source->wide);
  ScrProtoString *owner = source->owner ? (ScrProtoString *)source->owner : source;
  /* Adapted from QuickJS-NG js_sub_string (see vendor/quickjs-strings).
   * Large slices retain the root; small wide slices narrow to Latin-1 when
   * all their units fit, preventing a tiny result from retaining its parent. */
  bool view = SCR_PROTO_SHARED && (range.length << source->wide) > 1024;
  bool wide = source->wide;
  if (!view && wide) {
    uint16_t combined = 0;
    for (size_t i = 0; i < range.length; i++) combined |= ((const uint16_t *)units)[i];
    wide = combined > 0xff;
  }
  ScrProtoString *q = scr_proto_alloc(range.length, wide, view ? owner : NULL, units);
  q->ascii = source->ascii;
  q->json_short = source->json_short;
  if (q->json_short && q->wide) {
    const uint16_t *u = units;
    if ((u[0] & 0xfc00) == 0xdc00 || (u[range.length - 1] & 0xfc00) == 0xd800)
      q->json_short = false;
  }
  if (!view) {
    if (source->wide && !wide) {
      for (size_t i = 0; i < range.length; i++)
        ((uint8_t *)q->units)[i] = (uint8_t)((const uint16_t *)units)[i];
    } else memcpy((void *)q->units, units, range.length << wide);
  }
  return (ScrStr *)q;
}

static ScrStr *scr_proto_concat(ScrStr *a, ScrStr *b) {
  ScrProtoString *left = scr_proto_from_flat(a), *right = scr_proto_from_flat(b);
  size_t n = left->length + right->length;
  ScrProtoString *q = scr_proto_alloc(n, left->wide || right->wide, NULL, NULL);
  q->ascii = left->ascii && right->ascii;
  q->json_short = left->json_short && right->json_short;
  for (size_t i = 0; i < n; i++) {
    uint16_t unit = i < left->length ? scr_proto_unit(left, i) : scr_proto_unit(right, i - left->length);
    if (q->wide) ((uint16_t *)q->units)[i] = unit;
    else ((uint8_t *)q->units)[i] = (uint8_t)unit;
  }
  scr_str_release((ScrStr *)left); scr_str_release((ScrStr *)right);
  return (ScrStr *)q;
}

static ScrStr *scr_proto_concat_parts(ScrStr *const *parts, size_t count) {
  ScrStr *out = scr_str_empty();
  for (size_t i = 0; i < count; i++) {
    ScrStr *next = scr_proto_concat(out, parts[i]);
    scr_str_release(out); out = next;
  }
  return out;
}

static ScrStr *scr_proto_concat_f64(ScrStr *a, double x) {
  char text[32]; size_t n = (size_t)scr_f64_to_str(x, text);
  ScrStr *number = scr_str_new(text, n);
  ScrStr *out = scr_proto_concat(a, number);
  scr_str_release(number);
  return out;
}

static bool scr_proto_eq(ScrStr *a, ScrStr *b) {
  if (a == b) return true;
  ScrProtoString *left = scr_proto_from_flat(a), *right = scr_proto_from_flat(b);
  bool equal = left->length == right->length;
  if (equal) equal = scr_qjs_cmp(scr_proto_span(left), scr_proto_span(right), 0, 0, left->length) == 0;
  scr_str_release((ScrStr *)left); scr_str_release((ScrStr *)right);
  return equal;
}

static int scr_proto_cmp(ScrStr *a, ScrStr *b) {
  ScrProtoString *left = scr_proto_from_flat(a), *right = scr_proto_from_flat(b);
  size_t n = left->length < right->length ? left->length : right->length;
  int result = scr_qjs_cmp(scr_proto_span(left), scr_proto_span(right), 0, 0, n);
  if (!result) result = (left->length > right->length) - (left->length < right->length);
  scr_str_release((ScrStr *)left); scr_str_release((ScrStr *)right);
  return result;
}

static double scr_proto_find(ScrStr *s, ScrStr *needle, double position, bool last) {
  ScrProtoString *a = scr_proto_from_flat(s), *b = scr_proto_from_flat(needle);
  if (last && isnan(position)) position = INFINITY;
  size_t from = scr_str_clamp_u16_position(position, a->length);
  size_t found = SIZE_MAX;
  if (!last) found = scr_qjs_indexof(scr_proto_span(a), scr_proto_span(b), from);
  else if (b->length <= a->length) {
    size_t i = from < a->length - b->length ? from : a->length - b->length;
    for (;;) {
      if (!scr_qjs_cmp(scr_proto_span(a), scr_proto_span(b), i, 0, b->length)) { found = i; break; }
      if (i == 0) break;
      i--;
    }
  }
  scr_str_release((ScrStr *)a); scr_str_release((ScrStr *)b);
  return found == SIZE_MAX ? -1.0 : (double)found;
}

static bool scr_proto_affix(ScrStr *s, ScrStr *needle, double position, bool end) {
  ScrProtoString *a = scr_proto_from_flat(s), *b = scr_proto_from_flat(needle);
  size_t pos = scr_str_clamp_u16_position(position, a->length);
  bool fits = end ? b->length <= pos : b->length <= a->length - pos;
  size_t start = end && fits ? pos - b->length : pos;
  bool result = fits && !scr_qjs_cmp(scr_proto_span(a), scr_proto_span(b), start, 0, b->length);
  scr_str_release((ScrStr *)a); scr_str_release((ScrStr *)b);
  return result;
}

/* Append code units, widening Latin-1 or decoding legacy WTF-8 only when
 * needed. Native strings with matching widths use one memcpy. */
static size_t scr_proto_copy_units(ScrProtoString *out, size_t at, const ScrStr *s) {
  if (!s || !s->len) return at;
  if (scr_proto_is(s)) {
    const ScrProtoString *q = (const ScrProtoString *)s;
    if (out->wide == q->wide) {
      memcpy((char *)out->units + (at << out->wide), q->units, q->length << q->wide);
    } else {
      for (size_t i = 0; i < q->length; i++) ((uint16_t *)out->units)[at + i] = ((const uint8_t *)q->units)[i];
    }
    return at + q->length;
  }
  if (!out->wide) {
    memcpy((uint8_t *)out->units + at, s->data, s->len); /* flat ASCII */
    return at + s->len;
  }
  for (size_t i = 0; i < s->len;) {
    size_t adv;
    uint32_t cp = scr_utf8_decode(s->data + i, &adv);
    i += adv;
    if (cp > 0xffff) {
      ((uint16_t *)out->units)[at++] = (uint16_t)scr_cp_high(cp);
      cp = scr_cp_low(cp);
    }
    ((uint16_t *)out->units)[at++] = (uint16_t)cp;
  }
  return at;
}

ScrStr *scr_proto_arr_join(ScrArr *a, ScrStr *sep) {
#if SCR_PROTO_NATIVE_JOIN
  if (!a->len) return scr_str_empty();
  if (a->len == 1) {
    ScrStr *only = (ScrStr *)(uintptr_t)a->data[0];
    return only ? scr_str_retain(only) : scr_str_empty();
  }
  const size_t limit = SCR_STR_MAX_CAP / 3;
  size_t sep_len = (size_t)scr_str_utf16_len(sep);
  if (sep_len > limit / (a->len - 1)) scr_oom();
  size_t total = sep_len * (a->len - 1);
  bool wide = sep_len && (scr_proto_is(sep) ? ((ScrProtoString *)sep)->wide : !sep->ascii);
  bool ascii = !sep_len || (scr_proto_is(sep) ? ((ScrProtoString *)sep)->ascii : sep->ascii);
#if SCR_PROTO_COMPACT_JSON
  bool json_short = scr_proto_short_json_string(sep);
#endif
  for (size_t i = 0; i < a->len; i++) {
    const ScrStr *s = (const ScrStr *)(uintptr_t)a->data[i];
    if (!s) continue;
    size_t n = (size_t)scr_str_utf16_len(s);
    if (n > limit - total) scr_oom();
    total += n;
    if (n) wide |= scr_proto_is(s) ? ((const ScrProtoString *)s)->wide : !s->ascii;
    if (n) ascii &= scr_proto_is(s) ? ((const ScrProtoString *)s)->ascii : s->ascii;
#if SCR_PROTO_COMPACT_JSON
    json_short &= scr_proto_short_json_string(s);
#endif
  }
  if (!total) return scr_str_empty();
  ScrProtoString *out = scr_proto_alloc(total, wide, NULL, NULL);
  out->ascii = ascii;
#if SCR_PROTO_COMPACT_JSON
  out->json_short = json_short;
#endif
  size_t at = 0;
  for (size_t i = 0; i < a->len; i++) {
    if (i) at = scr_proto_copy_units(out, at, sep);
    at = scr_proto_copy_units(out, at, (const ScrStr *)(uintptr_t)a->data[i]);
  }
  if (at != total) scr_trap("prototype join length mismatch");
  return (ScrStr *)out;
#else
  ScrArr *flat = scr_arr_new(SCR_ELEM_STR, 0);
  for (size_t i = 0; i < a->len; i++) {
    ScrStr *value = (ScrStr *)(uintptr_t)a->data[i];
    ScrStr *copy = value ? scr_proto_flat(value) : scr_str_empty();
    scr_arr_push_ref(flat, copy); /* push consumes the +1 reference */
  }
  ScrStr *separator = scr_proto_flat(sep);
  ScrStr *out = scr_arr_join(flat, separator);
  scr_str_release(separator); scr_arr_release(flat);
  return out;
#endif
}

__attribute__((destructor)) static void scr_proto_report(void) {
  if (getenv("SCRIPTC_PROTO_STATS"))
    fprintf(stderr, "PROTO_STATS {\"live\":%zu,\"liveBytes\":%zu,\"peakBytes\":%zu,\"roots\":%zu,\"views\":%zu,\"materializedBytes\":%zu,\"directJsonBytes\":%zu,\"vectorUnits\":%zu,\"mixedUnits\":%zu,\"decodeScratchBytes\":%zu,\"directDecodeUnits\":%zu,\"builderCapacityBytes\":%zu,\"builderLengthBytes\":%zu,\"decodeLayoutBytes\":%zu,\"bulkDecodeUnits\":%zu,\"repeatOneReuses\":%zu}\n",
      scr_proto_live, scr_proto_live_bytes, scr_proto_peak_bytes, scr_proto_roots, scr_proto_views, scr_proto_materialized_bytes, scr_proto_json_bytes, scr_proto_vector_units, scr_proto_mixed_units, scr_proto_decode_scratch_bytes, scr_proto_direct_decode_units, scr_proto_builder_capacity_bytes, scr_proto_builder_length_bytes, scr_proto_decode_layout_bytes, scr_proto_bulk_decode_units, scr_proto_repeat_one_reuses);
}
#endif /* SCR_PROTO_STRING_IMPL */
