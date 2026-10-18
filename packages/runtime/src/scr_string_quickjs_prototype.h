/* PROTOTYPE ONLY: alternate strings for the measured helper operations.
 * cap == SCR_STR_MAX_CAP is reserved by this experiment; no normal object
 * approaching that size is supported. It is not a shipping ABI marker. */
#ifndef SCR_STRING_QUICKJS_PROTOTYPE_H
#define SCR_STRING_QUICKJS_PROTOTYPE_H
#ifndef SCR_PROTO_COMPACT_JSON
#define SCR_PROTO_COMPACT_JSON 1
#endif
static inline bool scr_proto_is(const ScrStr *s) {
  return SCR_QUICKJS_STRINGS && s && s->cap == SCR_STR_MAX_CAP;
}
ScrStr *scr_proto_flat(const ScrStr *s);
/* Raw output needs 3 * length bytes. JSON accepts the conservative
 * 6 * length + 2 bound or the metadata-aware scr_proto_json_capacity. */
size_t scr_proto_encode(const ScrStr *s, char *dst, bool json);
/* A borrowed native ASCII span; no UTF-8 materialization is needed. */
const char *scr_proto_ascii_data(const ScrStr *s, size_t *length);
void scr_proto_note_json(size_t bytes);
size_t scr_proto_json_capacity(const ScrStr *s);
void scr_proto_note_builder_capacity(size_t capacity, size_t length);
ScrStr *scr_proto_arr_join(ScrArr *a, ScrStr *sep);
#ifdef SCR_PROTO_STRING_IMPL
static void scr_proto_release(ScrStr *s);
static double scr_proto_char_code_at(ScrStr *s, double index);
static ScrStr *scr_proto_repeat(ScrStr *s, double n);
static ScrStr *scr_proto_slice(ScrStr *s, double from, double to, bool substring);
static void scr_proto_range(ScrStr *s, double from, double to, bool substring, ScrStringSlice *out);
static ScrStr *scr_proto_concat(ScrStr *a, ScrStr *b);
static ScrStr *scr_proto_concat_parts(ScrStr *const *parts, size_t count);
static ScrStr *scr_proto_concat_f64(ScrStr *a, double x);
static bool scr_proto_eq(ScrStr *a, ScrStr *b);
static int scr_proto_cmp(ScrStr *a, ScrStr *b);
static double scr_proto_find(ScrStr *s, ScrStr *needle, double position, bool last);
static bool scr_proto_affix(ScrStr *s, ScrStr *needle, double position, bool end);
#endif
#endif
