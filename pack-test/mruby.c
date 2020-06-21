/*
** array.c - Array class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/range.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>
#include "value_array.h"

#define ARY_DEFAULT_LEN   4
#define ARY_SHRINK_RATIO  5 /* must be larger than 2 */
#define ARY_C_MAX_SIZE (SIZE_MAX / sizeof(mrb_value))
#define ARY_MAX_SIZE ((mrb_int)((ARY_C_MAX_SIZE < (size_t)MRB_INT_MAX) ? ARY_C_MAX_SIZE : MRB_INT_MAX-1))

static struct RArray*
ary_new_capa(mrb_state *mrb, mrb_int capa)
{
  struct RArray *a;
  size_t blen;

  if (capa > ARY_MAX_SIZE) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
  blen = capa * sizeof(mrb_value);

  a = (struct RArray*)mrb_obj_alloc(mrb, MRB_TT_ARRAY, mrb->array_class);
  if (capa <= MRB_ARY_EMBED_LEN_MAX) {
    ARY_SET_EMBED_LEN(a, 0);
  }
  else {
    a->as.heap.ptr = (mrb_value *)mrb_malloc(mrb, blen);
    a->as.heap.aux.capa = capa;
    a->as.heap.len = 0;
  }

  return a;
}

MRB_API mrb_value
mrb_ary_new_capa(mrb_state *mrb, mrb_int capa)
{
  struct RArray *a = ary_new_capa(mrb, capa);
  return mrb_obj_value(a);
}

MRB_API mrb_value
mrb_ary_new(mrb_state *mrb)
{
  return mrb_ary_new_capa(mrb, 0);
}

/*
 * to copy array, use this instead of memcpy because of portability
 * * gcc on ARM may fail optimization of memcpy
 *   http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.faqs/ka3934.html
 * * gcc on MIPS also fail
 *   http://gcc.gnu.org/bugzilla/show_bug.cgi?id=39755
 * * memcpy doesn't exist on freestanding environment
 *
 * If you optimize for binary size, use memcpy instead of this at your own risk
 * of above portability issue.
 *
 * see also http://togetter.com/li/462898
 *
 */
static inline void
array_copy(mrb_value *dst, const mrb_value *src, mrb_int size)
{
  mrb_int i;

  for (i = 0; i < size; i++) {
    dst[i] = src[i];
  }
}

static struct RArray*
ary_new_from_values(mrb_state *mrb, mrb_int size, const mrb_value *vals)
{
  struct RArray *a = ary_new_capa(mrb, size);

  array_copy(ARY_PTR(a), vals, size);
  ARY_SET_LEN(a, size);

  return a;
}

MRB_API mrb_value
mrb_ary_new_from_values(mrb_state *mrb, mrb_int size, const mrb_value *vals)
{
  struct RArray *a = ary_new_from_values(mrb, size, vals);
  return mrb_obj_value(a);
}

MRB_API mrb_value
mrb_assoc_new(mrb_state *mrb, mrb_value car, mrb_value cdr)
{
  struct RArray *a;

  a = ary_new_capa(mrb, 2);
  ARY_PTR(a)[0] = car;
  ARY_PTR(a)[1] = cdr;
  ARY_SET_LEN(a, 2);
  return mrb_obj_value(a);
}

static void
ary_fill_with_nil(mrb_value *ptr, mrb_int size)
{
  mrb_value nil = mrb_nil_value();

  while (size--) {
    *ptr++ = nil;
  }
}

static void
ary_modify_check(mrb_state *mrb, struct RArray *a)
{
  mrb_check_frozen(mrb, a);
}

static void
ary_modify(mrb_state *mrb, struct RArray *a)
{
  ary_modify_check(mrb, a);

  if (ARY_SHARED_P(a)) {
    mrb_shared_array *shared = a->as.heap.aux.shared;

    if (shared->refcnt == 1 && a->as.heap.ptr == shared->ptr) {
      a->as.heap.ptr = shared->ptr;
      a->as.heap.aux.capa = a->as.heap.len;
      mrb_free(mrb, shared);
    }
    else {
      mrb_value *ptr, *p;
      mrb_int len;

      p = a->as.heap.ptr;
      len = a->as.heap.len * sizeof(mrb_value);
      ptr = (mrb_value *)mrb_malloc(mrb, len);
      if (p) {
        array_copy(ptr, p, a->as.heap.len);
      }
      a->as.heap.ptr = ptr;
      a->as.heap.aux.capa = a->as.heap.len;
      mrb_ary_decref(mrb, shared);
    }
    ARY_UNSET_SHARED_FLAG(a);
  }
}

MRB_API void
mrb_ary_modify(mrb_state *mrb, struct RArray* a)
{
  mrb_write_barrier(mrb, (struct RBasic*)a);
  ary_modify(mrb, a);
}

static void
ary_make_shared(mrb_state *mrb, struct RArray *a)
{
  if (!ARY_SHARED_P(a) && !ARY_EMBED_P(a)) {
    mrb_shared_array *shared = (mrb_shared_array *)mrb_malloc(mrb, sizeof(mrb_shared_array));
    mrb_value *ptr = a->as.heap.ptr;
    mrb_int len = a->as.heap.len;

    shared->refcnt = 1;
    if (a->as.heap.aux.capa > len) {
      a->as.heap.ptr = shared->ptr = (mrb_value *)mrb_realloc(mrb, ptr, sizeof(mrb_value)*len+1);
    }
    else {
      shared->ptr = ptr;
    }
    shared->len = len;
    a->as.heap.aux.shared = shared;
    ARY_SET_SHARED_FLAG(a);
  }
}

static void
ary_expand_capa(mrb_state *mrb, struct RArray *a, mrb_int len)
{
  mrb_int capa = ARY_CAPA(a);

  if (len > ARY_MAX_SIZE || len < 0) {
  size_error:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }

  if (capa < ARY_DEFAULT_LEN) {
    capa = ARY_DEFAULT_LEN;
  }
  while (capa < len) {
    if (capa <= ARY_MAX_SIZE / 2) {
      capa *= 2;
    }
    else {
      capa = len;
    }
  }
  if (capa < len || capa > ARY_MAX_SIZE) {
    goto size_error;
  }

  if (ARY_EMBED_P(a)) {
    mrb_value *ptr = ARY_EMBED_PTR(a);
    mrb_int len = ARY_EMBED_LEN(a);
    mrb_value *expanded_ptr = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value)*capa);

    ARY_UNSET_EMBED_FLAG(a);
    array_copy(expanded_ptr, ptr, len);
    a->as.heap.len = len;
    a->as.heap.aux.capa = capa;
    a->as.heap.ptr = expanded_ptr;
  }
  else if (capa > a->as.heap.aux.capa) {
    mrb_value *expanded_ptr = (mrb_value *)mrb_realloc(mrb, a->as.heap.ptr, sizeof(mrb_value)*capa);

    a->as.heap.aux.capa = capa;
    a->as.heap.ptr = expanded_ptr;
  }
}

static void
ary_shrink_capa(mrb_state *mrb, struct RArray *a)
{

  mrb_int capa;

  if (ARY_EMBED_P(a)) return;

  capa = a->as.heap.aux.capa;
  if (capa < ARY_DEFAULT_LEN * 2) return;
  if (capa <= a->as.heap.len * ARY_SHRINK_RATIO) return;

  do {
    capa /= 2;
    if (capa < ARY_DEFAULT_LEN) {
      capa = ARY_DEFAULT_LEN;
      break;
    }
  } while (capa > a->as.heap.len * ARY_SHRINK_RATIO);

  if (capa > a->as.heap.len && capa < a->as.heap.aux.capa) {
    a->as.heap.aux.capa = capa;
    a->as.heap.ptr = (mrb_value *)mrb_realloc(mrb, a->as.heap.ptr, sizeof(mrb_value)*capa);
  }
}

MRB_API mrb_value
mrb_ary_resize(mrb_state *mrb, mrb_value ary, mrb_int new_len)
{
  mrb_int old_len;
  struct RArray *a = mrb_ary_ptr(ary);

  ary_modify(mrb, a);
  old_len = RARRAY_LEN(ary);
  if (old_len != new_len) {
    if (new_len < old_len) {
      ary_shrink_capa(mrb, a);
    }
    else {
      ary_expand_capa(mrb, a, new_len);
      ary_fill_with_nil(ARY_PTR(a) + old_len, new_len - old_len);
    }
    ARY_SET_LEN(a, new_len);
  }

  return ary;
}

static mrb_value
mrb_ary_s_create(mrb_state *mrb, mrb_value klass)
{
  mrb_value ary;
  mrb_value *vals;
  mrb_int len;
  struct RArray *a;

  mrb_get_args(mrb, "*!", &vals, &len);
  ary = mrb_ary_new_from_values(mrb, len, vals);
  a = mrb_ary_ptr(ary);
  a->c = mrb_class_ptr(klass);

  return ary;
}

static void ary_replace(mrb_state*, struct RArray*, struct RArray*);

static void
ary_concat(mrb_state *mrb, struct RArray *a, struct RArray *a2)
{
  mrb_int len;

  if (ARY_LEN(a) == 0) {
    ary_replace(mrb, a, a2);
    return;
  }
  if (ARY_LEN(a2) > ARY_MAX_SIZE - ARY_LEN(a)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
  len = ARY_LEN(a) + ARY_LEN(a2);

  ary_modify(mrb, a);
  if (ARY_CAPA(a) < len) {
    ary_expand_capa(mrb, a, len);
  }
  array_copy(ARY_PTR(a)+ARY_LEN(a), ARY_PTR(a2), ARY_LEN(a2));
  mrb_write_barrier(mrb, (struct RBasic*)a);
  ARY_SET_LEN(a, len);
}

MRB_API void
mrb_ary_concat(mrb_state *mrb, mrb_value self, mrb_value other)
{
  struct RArray *a2 = mrb_ary_ptr(other);

  ary_concat(mrb, mrb_ary_ptr(self), a2);
}

static mrb_value
mrb_ary_concat_m(mrb_state *mrb, mrb_value self)
{
  mrb_value ary;

  mrb_get_args(mrb, "A", &ary);
  mrb_ary_concat(mrb, self, ary);
  return self;
}

static mrb_value
mrb_ary_plus(mrb_state *mrb, mrb_value self)
{
  struct RArray *a1 = mrb_ary_ptr(self);
  struct RArray *a2;
  mrb_value *ptr;
  mrb_int blen, len1;

  mrb_get_args(mrb, "a", &ptr, &blen);
  if (ARY_MAX_SIZE - blen < ARY_LEN(a1)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
  len1 = ARY_LEN(a1);
  a2 = ary_new_capa(mrb, len1 + blen);
  array_copy(ARY_PTR(a2), ARY_PTR(a1), len1);
  array_copy(ARY_PTR(a2) + len1, ptr, blen);
  ARY_SET_LEN(a2, len1+blen);

  return mrb_obj_value(a2);
}

#define ARY_REPLACE_SHARED_MIN 20

static void
ary_replace(mrb_state *mrb, struct RArray *a, struct RArray *b)
{
  mrb_int len = ARY_LEN(b);

  ary_modify_check(mrb, a);
  if (a == b) return;
  if (ARY_SHARED_P(a)) {
    mrb_ary_decref(mrb, a->as.heap.aux.shared);
    a->as.heap.aux.capa = 0;
    a->as.heap.len = 0;
    a->as.heap.ptr = NULL;
    ARY_UNSET_SHARED_FLAG(a);
  }
  if (ARY_SHARED_P(b)) {
  shared_b:
    if (ARY_EMBED_P(a)) {
      ARY_UNSET_EMBED_FLAG(a);
    }
    else {
      mrb_free(mrb, a->as.heap.ptr);
    }
    a->as.heap.ptr = b->as.heap.ptr;
    a->as.heap.len = len;
    a->as.heap.aux.shared = b->as.heap.aux.shared;
    a->as.heap.aux.shared->refcnt++;
    ARY_SET_SHARED_FLAG(a);
    mrb_write_barrier(mrb, (struct RBasic*)a);
    return;
  }
  if (!mrb_frozen_p(b) && len > ARY_REPLACE_SHARED_MIN) {
    ary_make_shared(mrb, b);
    goto shared_b;
  }
  if (ARY_CAPA(a) < len)
    ary_expand_capa(mrb, a, len);
  array_copy(ARY_PTR(a), ARY_PTR(b), len);
  mrb_write_barrier(mrb, (struct RBasic*)a);
  ARY_SET_LEN(a, len);
}

MRB_API void
mrb_ary_replace(mrb_state *mrb, mrb_value self, mrb_value other)
{
  struct RArray *a1 = mrb_ary_ptr(self);
  struct RArray *a2 = mrb_ary_ptr(other);

  if (a1 != a2) {
    ary_replace(mrb, a1, a2);
  }
}

static mrb_value
mrb_ary_replace_m(mrb_state *mrb, mrb_value self)
{
  mrb_value other;

  mrb_get_args(mrb, "A", &other);
  mrb_ary_replace(mrb, self, other);

  return self;
}

static mrb_value
mrb_ary_times(mrb_state *mrb, mrb_value self)
{
  struct RArray *a1 = mrb_ary_ptr(self);
  struct RArray *a2;
  mrb_value *ptr;
  mrb_int times, len1;

  mrb_get_args(mrb, "i", &times);
  if (times < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative argument");
  }
  if (times == 0) return mrb_ary_new(mrb);
  if (ARY_MAX_SIZE / times < ARY_LEN(a1)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
  len1 = ARY_LEN(a1);
  a2 = ary_new_capa(mrb, len1 * times);
  ARY_SET_LEN(a2, len1 * times);
  ptr = ARY_PTR(a2);
  while (times--) {
    array_copy(ptr, ARY_PTR(a1), len1);
    ptr += len1;
  }

  return mrb_obj_value(a2);
}

static mrb_value
mrb_ary_reverse_bang(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int len = ARY_LEN(a);

  if (len > 1) {
    mrb_value *p1, *p2;

    ary_modify(mrb, a);
    p1 = ARY_PTR(a);
    p2 = p1 + len - 1;

    while (p1 < p2) {
      mrb_value tmp = *p1;
      *p1++ = *p2;
      *p2-- = tmp;
    }
  }
  return self;
}

static mrb_value
mrb_ary_reverse(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self), *b = ary_new_capa(mrb, ARY_LEN(a));
  mrb_int len = ARY_LEN(a);

  if (len > 0) {
    mrb_value *p1, *p2, *e;

    p1 = ARY_PTR(a);
    e  = p1 + len;
    p2 = ARY_PTR(b) + len - 1;
    while (p1 < e) {
      *p2-- = *p1++;
    }
    ARY_SET_LEN(b, len);
  }
  return mrb_obj_value(b);
}

MRB_API void
mrb_ary_push(mrb_state *mrb, mrb_value ary, mrb_value elem)
{
  struct RArray *a = mrb_ary_ptr(ary);
  mrb_int len = ARY_LEN(a);

  ary_modify(mrb, a);
  if (len == ARY_CAPA(a))
    ary_expand_capa(mrb, a, len + 1);
  ARY_PTR(a)[len] = elem;
  ARY_SET_LEN(a, len+1);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)a, elem);
}

static mrb_value
mrb_ary_push_m(mrb_state *mrb, mrb_value self)
{
  mrb_value *argv;
  mrb_int len, len2, alen;
  struct RArray *a;

  mrb_get_args(mrb, "*!", &argv, &alen);
  a = mrb_ary_ptr(self);
  ary_modify(mrb, a);
  len = ARY_LEN(a);
  len2 = len + alen;
  if (ARY_CAPA(a) < len2) {
    ary_expand_capa(mrb, a, len2);
  }
  array_copy(ARY_PTR(a)+len, argv, alen);
  ARY_SET_LEN(a, len2);
  mrb_write_barrier(mrb, (struct RBasic*)a);

  return self;
}

MRB_API mrb_value
mrb_ary_pop(mrb_state *mrb, mrb_value ary)
{
  struct RArray *a = mrb_ary_ptr(ary);
  mrb_int len = ARY_LEN(a);

  ary_modify_check(mrb, a);
  if (len == 0) return mrb_nil_value();
  ARY_SET_LEN(a, len-1);
  return ARY_PTR(a)[len-1];
}

#define ARY_SHIFT_SHARED_MIN 10

MRB_API mrb_value
mrb_ary_shift(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int len = ARY_LEN(a);
  mrb_value val;

  ary_modify_check(mrb, a);
  if (len == 0) return mrb_nil_value();
  if (ARY_SHARED_P(a)) {
  L_SHIFT:
    val = a->as.heap.ptr[0];
    a->as.heap.ptr++;
    a->as.heap.len--;
    return val;
  }
  if (len > ARY_SHIFT_SHARED_MIN) {
    ary_make_shared(mrb, a);
    goto L_SHIFT;
  }
  else {
    mrb_value *ptr = ARY_PTR(a);
    mrb_int size = len;

    val = *ptr;
    while (--size) {
      *ptr = *(ptr+1);
      ++ptr;
    }
    ARY_SET_LEN(a, len-1);
  }
  return val;
}

/* self = [1,2,3]
   item = 0
   self.unshift item
   p self #=> [0, 1, 2, 3] */
MRB_API mrb_value
mrb_ary_unshift(mrb_state *mrb, mrb_value self, mrb_value item)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int len = ARY_LEN(a);

  if (ARY_SHARED_P(a)
      && a->as.heap.aux.shared->refcnt == 1 /* shared only referenced from this array */
      && a->as.heap.ptr - a->as.heap.aux.shared->ptr >= 1) /* there's room for unshifted item */ {
    a->as.heap.ptr--;
    a->as.heap.ptr[0] = item;
  }
  else {
    mrb_value *ptr;

    ary_modify(mrb, a);
    if (ARY_CAPA(a) < len + 1)
      ary_expand_capa(mrb, a, len + 1);
    ptr = ARY_PTR(a);
    value_move(ptr + 1, ptr, len);
    ptr[0] = item;
  }
  ARY_SET_LEN(a, len+1);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)a, item);

  return self;
}

static mrb_value
mrb_ary_unshift_m(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_value *vals, *ptr;
  mrb_int alen, len;

  mrb_get_args(mrb, "*!", &vals, &alen);
  if (alen == 0) {
    ary_modify_check(mrb, a);
    return self;
  }
  len = ARY_LEN(a);
  if (alen > ARY_MAX_SIZE - len) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
  if (ARY_SHARED_P(a)
      && a->as.heap.aux.shared->refcnt == 1 /* shared only referenced from this array */
      && a->as.heap.ptr - a->as.heap.aux.shared->ptr >= alen) /* there's room for unshifted item */ {
    ary_modify_check(mrb, a);
    a->as.heap.ptr -= alen;
    ptr = a->as.heap.ptr;
  }
  else {
    mrb_bool same = vals == ARY_PTR(a);
    ary_modify(mrb, a);
    if (ARY_CAPA(a) < len + alen)
      ary_expand_capa(mrb, a, len + alen);
    ptr = ARY_PTR(a);
    value_move(ptr + alen, ptr, len);
    if (same) vals = ptr;
  }
  array_copy(ptr, vals, alen);
  ARY_SET_LEN(a, len+alen);
  while (alen--) {
    mrb_field_write_barrier_value(mrb, (struct RBasic*)a, vals[alen]);
  }

  return self;
}

MRB_API mrb_value
mrb_ary_ref(mrb_state *mrb, mrb_value ary, mrb_int n)
{
  struct RArray *a = mrb_ary_ptr(ary);
  mrb_int len = ARY_LEN(a);

  /* range check */
  if (n < 0) n += len;
  if (n < 0 || len <= n) return mrb_nil_value();

  return ARY_PTR(a)[n];
}

MRB_API void
mrb_ary_set(mrb_state *mrb, mrb_value ary, mrb_int n, mrb_value val)
{
  struct RArray *a = mrb_ary_ptr(ary);
  mrb_int len = ARY_LEN(a);

  ary_modify(mrb, a);
  /* range check */
  if (n < 0) {
    n += len;
    if (n < 0) {
      mrb_raisef(mrb, E_INDEX_ERROR, "index %i out of array", n - len);
    }
  }
  if (len <= n) {
    if (ARY_CAPA(a) <= n)
      ary_expand_capa(mrb, a, n + 1);
    ary_fill_with_nil(ARY_PTR(a) + len, n + 1 - len);
    ARY_SET_LEN(a, n+1);
  }

  ARY_PTR(a)[n] = val;
  mrb_field_write_barrier_value(mrb, (struct RBasic*)a, val);
}

static struct RArray*
ary_dup(mrb_state *mrb, struct RArray *a)
{
  return ary_new_from_values(mrb, ARY_LEN(a), ARY_PTR(a));
}

MRB_API mrb_value
mrb_ary_splice(mrb_state *mrb, mrb_value ary, mrb_int head, mrb_int len, mrb_value rpl)
{
  struct RArray *a = mrb_ary_ptr(ary);
  mrb_int alen = ARY_LEN(a);
  const mrb_value *argv;
  mrb_int argc;
  mrb_int tail;

  ary_modify(mrb, a);

  /* len check */
  if (len < 0) mrb_raisef(mrb, E_INDEX_ERROR, "negative length (%i)", len);

  /* range check */
  if (head < 0) {
    head += alen;
    if (head < 0) {
      mrb_raise(mrb, E_INDEX_ERROR, "index is out of array");
    }
  }
  tail = head + len;
  if (alen < len || alen < tail) {
    len = alen - head;
  }

  /* size check */
  if (mrb_array_p(rpl)) {
    argc = RARRAY_LEN(rpl);
    argv = RARRAY_PTR(rpl);
    if (argv == ARY_PTR(a)) {
      struct RArray *r;

      if (argc > 32767) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "too big recursive splice");
      }
      r = ary_dup(mrb, a);
      argv = ARY_PTR(r);
    }
  }
  else if (mrb_undef_p(rpl)) {
    argc = 0;
    argv = NULL;
  }
  else {
    argc = 1;
    argv = &rpl;
  }
  if (head >= alen) {
    if (head > ARY_MAX_SIZE - argc) {
      mrb_raisef(mrb, E_INDEX_ERROR, "index %i too big", head);
    }
    len = head + argc;
    if (len > ARY_CAPA(a)) {
      ary_expand_capa(mrb, a, head + argc);
    }
    ary_fill_with_nil(ARY_PTR(a) + alen, head - alen);
    if (argc > 0) {
      array_copy(ARY_PTR(a) + head, argv, argc);
    }
    ARY_SET_LEN(a, len);
  }
  else {
    mrb_int newlen;

    if (alen - len > ARY_MAX_SIZE - argc) {
      mrb_raisef(mrb, E_INDEX_ERROR, "index %i too big", alen + argc - len);
    }
    newlen = alen + argc - len;
    if (newlen > ARY_CAPA(a)) {
      ary_expand_capa(mrb, a, newlen);
    }

    if (len != argc) {
      mrb_value *ptr = ARY_PTR(a);
      tail = head + len;
      value_move(ptr + head + argc, ptr + tail, alen - tail);
      ARY_SET_LEN(a, newlen);
    }
    if (argc > 0) {
      value_move(ARY_PTR(a) + head, argv, argc);
    }
  }
  mrb_write_barrier(mrb, (struct RBasic*)a);
  return ary;
}

void
mrb_ary_decref(mrb_state *mrb, mrb_shared_array *shared)
{
  shared->refcnt--;
  if (shared->refcnt == 0) {
    mrb_free(mrb, shared->ptr);
    mrb_free(mrb, shared);
  }
}

static mrb_value
ary_subseq(mrb_state *mrb, struct RArray *a, mrb_int beg, mrb_int len)
{
  struct RArray *b;

  if (!ARY_SHARED_P(a) && len <= ARY_SHIFT_SHARED_MIN) {
    return mrb_ary_new_from_values(mrb, len, ARY_PTR(a)+beg);
  }
  ary_make_shared(mrb, a);
  b  = (struct RArray*)mrb_obj_alloc(mrb, MRB_TT_ARRAY, mrb->array_class);
  b->as.heap.ptr = a->as.heap.ptr + beg;
  b->as.heap.len = len;
  b->as.heap.aux.shared = a->as.heap.aux.shared;
  b->as.heap.aux.shared->refcnt++;
  ARY_SET_SHARED_FLAG(b);

  return mrb_obj_value(b);
}

mrb_value
mrb_ary_subseq(mrb_state *mrb, mrb_value ary, mrb_int beg, mrb_int len)
{
  struct RArray *a = mrb_ary_ptr(ary);
  return ary_subseq(mrb, a, beg, len);
}

static mrb_int
aget_index(mrb_state *mrb, mrb_value index)
{
  if (mrb_fixnum_p(index)) {
    return mrb_fixnum(index);
  }
#ifndef MRB_WITHOUT_FLOAT
  else if (mrb_float_p(index)) {
    return (mrb_int)mrb_float(index);
  }
#endif
  else {
    mrb_int i, argc;
    mrb_value *argv;

    mrb_get_args(mrb, "i*!", &i, &argv, &argc);
    return i;
  }
}

/*
 *  call-seq:
 *     ary[index]                -> obj     or nil
 *     ary[start, length]        -> new_ary or nil
 *     ary[range]                -> new_ary or nil
 *     ary.slice(index)          -> obj     or nil
 *     ary.slice(start, length)  -> new_ary or nil
 *     ary.slice(range)          -> new_ary or nil
 *
 *  Element Reference --- Returns the element at +index+, or returns a
 *  subarray starting at the +start+ index and continuing for +length+
 *  elements, or returns a subarray specified by +range+ of indices.
 *
 *  Negative indices count backward from the end of the array (-1 is the last
 *  element).  For +start+ and +range+ cases the starting index is just before
 *  an element.  Additionally, an empty array is returned when the starting
 *  index for an element range is at the end of the array.
 *
 *  Returns +nil+ if the index (or starting index) are out of range.
 *
 *  a = [ "a", "b", "c", "d", "e" ]
 *  a[1]     => "b"
 *  a[1,2]   => ["b", "c"]
 *  a[1..-2] => ["b", "c", "d"]
 *
 */

static mrb_value
mrb_ary_aget(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int i, len, alen;
  mrb_value index;

  if (mrb_get_args(mrb, "o|i", &index, &len) == 1) {
    switch (mrb_type(index)) {
      /* a[n..m] */
    case MRB_TT_RANGE:
      if (mrb_range_beg_len(mrb, index, &i, &len, ARY_LEN(a), TRUE) == MRB_RANGE_OK) {
        return ary_subseq(mrb, a, i, len);
      }
      else {
        return mrb_nil_value();
      }
    case MRB_TT_FIXNUM:
      return mrb_ary_ref(mrb, self, mrb_fixnum(index));
    default:
      return mrb_ary_ref(mrb, self, aget_index(mrb, index));
    }
  }

  i = aget_index(mrb, index);
  alen = ARY_LEN(a);
  if (i < 0) i += alen;
  if (i < 0 || alen < i) return mrb_nil_value();
  if (len < 0) return mrb_nil_value();
  if (alen == i) return mrb_ary_new(mrb);
  if (len > alen - i) len = alen - i;

  return ary_subseq(mrb, a, i, len);
}

/*
 *  call-seq:
 *     ary[index]         = obj                      ->  obj
 *     ary[start, length] = obj or other_ary or nil  ->  obj or other_ary or nil
 *     ary[range]         = obj or other_ary or nil  ->  obj or other_ary or nil
 *
 *  Element Assignment --- Sets the element at +index+, or replaces a subarray
 *  from the +start+ index for +length+ elements, or replaces a subarray
 *  specified by the +range+ of indices.
 *
 *  If indices are greater than the current capacity of the array, the array
 *  grows automatically.  Elements are inserted into the array at +start+ if
 *  +length+ is zero.
 *
 *  Negative indices will count backward from the end of the array.  For
 *  +start+ and +range+ cases the starting index is just before an element.
 *
 *  An IndexError is raised if a negative index points past the beginning of
 *  the array.
 *
 *  See also Array#push, and Array#unshift.
 *
 *     a = Array.new
 *     a[4] = "4";                 #=> [nil, nil, nil, nil, "4"]
 *     a[0, 3] = [ 'a', 'b', 'c' ] #=> ["a", "b", "c", nil, "4"]
 *     a[1..2] = [ 1, 2 ]          #=> ["a", 1, 2, nil, "4"]
 *     a[0, 2] = "?"               #=> ["?", 2, nil, "4"]
 *     a[0..2] = "A"               #=> ["A", "4"]
 *     a[-1]   = "Z"               #=> ["A", "Z"]
 *     a[1..-1] = nil              #=> ["A", nil]
 *     a[1..-1] = []               #=> ["A"]
 *     a[0, 0] = [ 1, 2 ]          #=> [1, 2, "A"]
 *     a[3, 0] = "B"               #=> [1, 2, "A", "B"]
 */

static mrb_value
mrb_ary_aset(mrb_state *mrb, mrb_value self)
{
  mrb_value v1, v2, v3;
  mrb_int i, len;

  mrb_ary_modify(mrb, mrb_ary_ptr(self));
  if (mrb_get_args(mrb, "oo|o", &v1, &v2, &v3) == 2) {
    /* a[n..m] = v */
    switch (mrb_range_beg_len(mrb, v1, &i, &len, RARRAY_LEN(self), FALSE)) {
    case MRB_RANGE_TYPE_MISMATCH:
      mrb_ary_set(mrb, self, aget_index(mrb, v1), v2);
      break;
    case MRB_RANGE_OK:
      mrb_ary_splice(mrb, self, i, len, v2);
      break;
    case MRB_RANGE_OUT:
      mrb_raisef(mrb, E_RANGE_ERROR, "%v out of range", v1);
      break;
    }
    return v2;
  }

  /* a[n,m] = v */
  mrb_ary_splice(mrb, self, aget_index(mrb, v1), aget_index(mrb, v2), v3);
  return v3;
}

static mrb_value
mrb_ary_delete_at(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int   index;
  mrb_value val;
  mrb_value *ptr;
  mrb_int len, alen;

  mrb_get_args(mrb, "i", &index);
  alen = ARY_LEN(a);
  if (index < 0) index += alen;
  if (index < 0 || alen <= index) return mrb_nil_value();

  ary_modify(mrb, a);
  ptr = ARY_PTR(a);
  val = ptr[index];

  ptr += index;
  len = alen - index;
  while (--len) {
    *ptr = *(ptr+1);
    ++ptr;
  }
  ARY_SET_LEN(a, alen-1);

  ary_shrink_capa(mrb, a);

  return val;
}

static mrb_value
mrb_ary_first(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int size, alen;

  if (mrb_get_argc(mrb) == 0) {
    return (ARY_LEN(a) > 0)? ARY_PTR(a)[0]: mrb_nil_value();
  }
  mrb_get_args(mrb, "|i", &size);
  if (size < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative array size");
  }

  alen = ARY_LEN(a);
  if (size > alen) size = alen;
  if (ARY_SHARED_P(a)) {
    return ary_subseq(mrb, a, 0, size);
  }
  return mrb_ary_new_from_values(mrb, size, ARY_PTR(a));
}

static mrb_value
mrb_ary_last(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int n, size, alen;

  n = mrb_get_args(mrb, "|i", &size);
  alen = ARY_LEN(a);
  if (n == 0) {
    return (alen > 0) ? ARY_PTR(a)[alen - 1]: mrb_nil_value();
  }

  if (size < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative array size");
  }
  if (size > alen) size = alen;
  if (ARY_SHARED_P(a) || size > ARY_DEFAULT_LEN) {
    return ary_subseq(mrb, a, alen - size, size);
  }
  return mrb_ary_new_from_values(mrb, size, ARY_PTR(a) + alen - size);
}

static mrb_value
mrb_ary_index_m(mrb_state *mrb, mrb_value self)
{
  mrb_value obj = mrb_get_arg1(mrb);
  mrb_int i;

  for (i = 0; i < RARRAY_LEN(self); i++) {
    if (mrb_equal(mrb, RARRAY_PTR(self)[i], obj)) {
      return mrb_fixnum_value(i);
    }
  }
  return mrb_nil_value();
}

static mrb_value
mrb_ary_rindex_m(mrb_state *mrb, mrb_value self)
{
  mrb_value obj = mrb_get_arg1(mrb);
  mrb_int i, len;

  for (i = RARRAY_LEN(self) - 1; i >= 0; i--) {
    if (mrb_equal(mrb, RARRAY_PTR(self)[i], obj)) {
      return mrb_fixnum_value(i);
    }
    if (i > (len = RARRAY_LEN(self))) {
      i = len;
    }
  }
  return mrb_nil_value();
}

MRB_API mrb_value
mrb_ary_splat(mrb_state *mrb, mrb_value v)
{
  mrb_value a;

  if (mrb_array_p(v)) {
    return v;
  }

  if (!mrb_respond_to(mrb, v, mrb_intern_lit(mrb, "to_a"))) {
    return mrb_ary_new_from_values(mrb, 1, &v);
  }

  a = mrb_funcall(mrb, v, "to_a", 0);
  if (mrb_nil_p(a)) {
    return mrb_ary_new_from_values(mrb, 1, &v);
  }
  mrb_ensure_array_type(mrb, a);
  return a;
}

static mrb_value
mrb_ary_size(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  return mrb_fixnum_value(ARY_LEN(a));
}

MRB_API mrb_value
mrb_ary_clear(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  ary_modify(mrb, a);
  if (ARY_SHARED_P(a)) {
    mrb_ary_decref(mrb, a->as.heap.aux.shared);
    ARY_UNSET_SHARED_FLAG(a);
  }
  else if (!ARY_EMBED_P(a)){
    mrb_free(mrb, a->as.heap.ptr);
  }
  ARY_SET_EMBED_LEN(a, 0);

  return self;
}

static mrb_value
mrb_ary_clear_m(mrb_state *mrb, mrb_value self)
{
  return mrb_ary_clear(mrb, self);
}

static mrb_value
mrb_ary_empty_p(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  return mrb_bool_value(ARY_LEN(a) == 0);
}

MRB_API mrb_value
mrb_ary_entry(mrb_value ary, mrb_int offset)
{
  if (offset < 0) {
    offset += RARRAY_LEN(ary);
  }
  if (offset < 0 || RARRAY_LEN(ary) <= offset) {
    return mrb_nil_value();
  }
  return RARRAY_PTR(ary)[offset];
}

static mrb_value
join_ary(mrb_state *mrb, mrb_value ary, mrb_value sep, mrb_value list)
{
  mrb_int i;
  mrb_value result, val, tmp;

  /* check recursive */
  for (i=0; i<RARRAY_LEN(list); i++) {
    if (mrb_obj_equal(mrb, ary, RARRAY_PTR(list)[i])) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "recursive array join");
    }
  }

  mrb_ary_push(mrb, list, ary);

  result = mrb_str_new_capa(mrb, 64);

  for (i=0; i<RARRAY_LEN(ary); i++) {
    if (i > 0 && !mrb_nil_p(sep)) {
      mrb_str_cat_str(mrb, result, sep);
    }

    val = RARRAY_PTR(ary)[i];
    switch (mrb_type(val)) {
    case MRB_TT_ARRAY:
    ary_join:
      val = join_ary(mrb, val, sep, list);
      /* fall through */

    case MRB_TT_STRING:
    str_join:
      mrb_str_cat_str(mrb, result, val);
      break;

    default:
      if (!mrb_immediate_p(val)) {
        tmp = mrb_check_string_type(mrb, val);
        if (!mrb_nil_p(tmp)) {
          val = tmp;
          goto str_join;
        }
        tmp = mrb_check_array_type(mrb, val);
        if (!mrb_nil_p(tmp)) {
          val = tmp;
          goto ary_join;
        }
      }
      val = mrb_obj_as_string(mrb, val);
      goto str_join;
    }
  }

  mrb_ary_pop(mrb, list);

  return result;
}

MRB_API mrb_value
mrb_ary_join(mrb_state *mrb, mrb_value ary, mrb_value sep)
{
  if (!mrb_nil_p(sep)) {
    sep = mrb_obj_as_string(mrb, sep);
  }
  return join_ary(mrb, ary, sep, mrb_ary_new(mrb));
}

/*
 *  call-seq:
 *     ary.join(sep="")    -> str
 *
 *  Returns a string created by converting each element of the array to
 *  a string, separated by <i>sep</i>.
 *
 *     [ "a", "b", "c" ].join        #=> "abc"
 *     [ "a", "b", "c" ].join("-")   #=> "a-b-c"
 */

static mrb_value
mrb_ary_join_m(mrb_state *mrb, mrb_value ary)
{
  mrb_value sep = mrb_nil_value();

  mrb_get_args(mrb, "|S!", &sep);
  return mrb_ary_join(mrb, ary, sep);
}

static mrb_value
mrb_ary_eq(mrb_state *mrb, mrb_value ary1)
{
  mrb_value ary2 = mrb_get_arg1(mrb);

  if (mrb_obj_equal(mrb, ary1, ary2)) return mrb_true_value();
  if (!mrb_array_p(ary2)) {
    return mrb_false_value();
  }
  if (RARRAY_LEN(ary1) != RARRAY_LEN(ary2)) return mrb_false_value();

  return ary2;
}

static mrb_value
mrb_ary_cmp(mrb_state *mrb, mrb_value ary1)
{
  mrb_value ary2 = mrb_get_arg1(mrb);

  if (mrb_obj_equal(mrb, ary1, ary2)) return mrb_fixnum_value(0);
  if (!mrb_array_p(ary2)) {
    return mrb_nil_value();
  }

  return ary2;
}

/* internal method to convert multi-value to single value */
static mrb_value
mrb_ary_svalue(mrb_state *mrb, mrb_value ary)
{
  switch (RARRAY_LEN(ary)) {
  case 0:
    return mrb_nil_value();
  case 1:
    return RARRAY_PTR(ary)[0];
  default:
    return ary;
  }
}

static const mrb_code each_iseq[] = {
  OP_ENTER, 0x0, 0x00, 0x1,  /* OP_ENTER     0:0:0:0:0:0:1 */
  OP_JMPIF, 0x1, 0x0, 19,    /* OP_JMPIF     R1  19 */
  OP_LOADSELF, 0x3,          /* OP_LOADSELF  R3 */
  OP_LOADSYM, 0x4, 0x0,      /* OP_LOADSYM   R4  :each*/
  OP_SEND, 0x3, 0x1, 0x1,    /* OP_SEND      R3  :to_enum   1 */
  OP_RETURN, 0x3,            /* OP_RETURN    R3 */
  OP_LOADI_0, 0x2,           /* OP_LOADI_0   R2 */
  OP_JMP, 0x0, 43,           /* OP_JMP       49 */
  OP_MOVE, 0x3, 0x1,         /* OP_MOVE      R3  R1 */
  OP_LOADSELF, 0x4,          /* OP_LOADSELF  R4 */
  OP_MOVE, 0x5, 0x2,         /* OP_MOVE      R5  R2 */
  OP_SEND, 0x4, 0x2, 0x1,    /* OP_SEND      R4  :[]        1 */
  OP_SEND, 0x3, 0x3, 0x1,    /* OP_SEND      R3  :call      1 */
  OP_ADDI, 0x2, 1,           /* OP_ADDI      R3  1 */
  OP_MOVE, 0x3, 0x2,         /* OP_MOVE      R3  R2 */
  OP_LOADSELF, 0x4,          /* OP_LOADSELF  R4 */
  OP_SEND, 0x4, 0x4, 0x0,    /* OP_SEND      R4  :length    0 */
  OP_LT, 0x3,                /* OP_LT        R3 */
  OP_JMPIF, 0x3, 0x0, 24,    /* OP_JMPIF     R3  24 */
  OP_RETURN, 0x0             /* OP_RETURN    R3 */
};

static void
init_ary_each(mrb_state *mrb, struct RClass *ary)
{
  struct RProc *p;
  mrb_method_t m;
  mrb_irep *each_irep = (mrb_irep*)mrb_malloc(mrb, sizeof(mrb_irep));
  static const mrb_irep mrb_irep_zero = { 0 };

  *each_irep = mrb_irep_zero;
  each_irep->syms = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym)*5);
  each_irep->syms[0] = mrb_intern_lit(mrb, "each");
  each_irep->syms[1] = mrb_intern_lit(mrb, "to_enum");
  each_irep->syms[2] = mrb_intern_lit(mrb, "[]");
  each_irep->syms[3] = mrb_intern_lit(mrb, "call");
  each_irep->syms[4] = mrb_intern_lit(mrb, "length");
  each_irep->slen = 5;
  each_irep->flags = MRB_ISEQ_NO_FREE;
  each_irep->iseq = each_iseq;
  each_irep->ilen = sizeof(each_iseq);
  each_irep->nregs = 7;
  each_irep->nlocals = 3;
  p = mrb_proc_new(mrb, each_irep);
  p->flags |= MRB_PROC_SCOPE | MRB_PROC_STRICT;
  MRB_METHOD_FROM_PROC(m, p);
  mrb_define_method_raw(mrb, ary, mrb_intern_lit(mrb, "each"), m);
}

void
mrb_init_array(mrb_state *mrb)
{
  struct RClass *a;

  mrb->array_class = a = mrb_define_class(mrb, "Array", mrb->object_class);              /* 15.2.12 */
  MRB_SET_INSTANCE_TT(a, MRB_TT_ARRAY);

  mrb_define_class_method(mrb, a, "[]",        mrb_ary_s_create,     MRB_ARGS_ANY());    /* 15.2.12.4.1 */

  mrb_define_method(mrb, a, "+",               mrb_ary_plus,         MRB_ARGS_REQ(1));   /* 15.2.12.5.1  */
  mrb_define_method(mrb, a, "*",               mrb_ary_times,        MRB_ARGS_REQ(1));   /* 15.2.12.5.2  */
  mrb_define_method(mrb, a, "<<",              mrb_ary_push_m,       MRB_ARGS_REQ(1));   /* 15.2.12.5.3  */
  mrb_define_method(mrb, a, "[]",              mrb_ary_aget,         MRB_ARGS_ARG(1,1)); /* 15.2.12.5.4  */
  mrb_define_method(mrb, a, "[]=",             mrb_ary_aset,         MRB_ARGS_ARG(2,1)); /* 15.2.12.5.5  */
  mrb_define_method(mrb, a, "clear",           mrb_ary_clear_m,      MRB_ARGS_NONE());   /* 15.2.12.5.6  */
  mrb_define_method(mrb, a, "concat",          mrb_ary_concat_m,     MRB_ARGS_REQ(1));   /* 15.2.12.5.8  */
  mrb_define_method(mrb, a, "delete_at",       mrb_ary_delete_at,    MRB_ARGS_REQ(1));   /* 15.2.12.5.9  */
  mrb_define_method(mrb, a, "empty?",          mrb_ary_empty_p,      MRB_ARGS_NONE());   /* 15.2.12.5.12 */
  mrb_define_method(mrb, a, "first",           mrb_ary_first,        MRB_ARGS_OPT(1));   /* 15.2.12.5.13 */
  mrb_define_method(mrb, a, "index",           mrb_ary_index_m,      MRB_ARGS_REQ(1));   /* 15.2.12.5.14 */
  mrb_define_method(mrb, a, "initialize_copy", mrb_ary_replace_m,    MRB_ARGS_REQ(1));   /* 15.2.12.5.16 */
  mrb_define_method(mrb, a, "join",            mrb_ary_join_m,       MRB_ARGS_OPT(1));   /* 15.2.12.5.17 */
  mrb_define_method(mrb, a, "last",            mrb_ary_last,         MRB_ARGS_OPT(1));   /* 15.2.12.5.18 */
  mrb_define_method(mrb, a, "length",          mrb_ary_size,         MRB_ARGS_NONE());   /* 15.2.12.5.19 */
  mrb_define_method(mrb, a, "pop",             mrb_ary_pop,          MRB_ARGS_NONE());   /* 15.2.12.5.21 */
  mrb_define_method(mrb, a, "push",            mrb_ary_push_m,       MRB_ARGS_ANY());    /* 15.2.12.5.22 */
  mrb_define_method(mrb, a, "replace",         mrb_ary_replace_m,    MRB_ARGS_REQ(1));   /* 15.2.12.5.23 */
  mrb_define_method(mrb, a, "reverse",         mrb_ary_reverse,      MRB_ARGS_NONE());   /* 15.2.12.5.24 */
  mrb_define_method(mrb, a, "reverse!",        mrb_ary_reverse_bang, MRB_ARGS_NONE());   /* 15.2.12.5.25 */
  mrb_define_method(mrb, a, "rindex",          mrb_ary_rindex_m,     MRB_ARGS_REQ(1));   /* 15.2.12.5.26 */
  mrb_define_method(mrb, a, "shift",           mrb_ary_shift,        MRB_ARGS_NONE());   /* 15.2.12.5.27 */
  mrb_define_method(mrb, a, "size",            mrb_ary_size,         MRB_ARGS_NONE());   /* 15.2.12.5.28 */
  mrb_define_method(mrb, a, "slice",           mrb_ary_aget,         MRB_ARGS_ARG(1,1)); /* 15.2.12.5.29 */
  mrb_define_method(mrb, a, "unshift",         mrb_ary_unshift_m,    MRB_ARGS_ANY());    /* 15.2.12.5.30 */

  mrb_define_method(mrb, a, "__ary_eq",        mrb_ary_eq,           MRB_ARGS_REQ(1));
  mrb_define_method(mrb, a, "__ary_cmp",       mrb_ary_cmp,          MRB_ARGS_REQ(1));
  mrb_define_method(mrb, a, "__ary_index",     mrb_ary_index_m,      MRB_ARGS_REQ(1));   /* kept for mruby-array-ext */
  mrb_define_method(mrb, a, "__svalue",        mrb_ary_svalue,       MRB_ARGS_NONE());

  init_ary_each(mrb, a);
}
/*
** backtrace.c -
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/variable.h>
#include <mruby/proc.h>
#include <mruby/array.h>
#include <mruby/string.h>
#include <mruby/class.h>
#include <mruby/debug.h>
#include <mruby/error.h>
#include <mruby/numeric.h>
#include <mruby/data.h>

struct backtrace_location {
  int32_t lineno;
  mrb_sym method_id;
  const char *filename;
};

typedef void (*each_backtrace_func)(mrb_state*, const struct backtrace_location*, void*);

static const mrb_data_type bt_type = { "Backtrace", mrb_free };

mrb_value mrb_exc_inspect(mrb_state *mrb, mrb_value exc);
mrb_value mrb_unpack_backtrace(mrb_state *mrb, mrb_value backtrace);

static void
each_backtrace(mrb_state *mrb, ptrdiff_t ciidx, const mrb_code *pc0, each_backtrace_func func, void *data)
{
  ptrdiff_t i;

  if (ciidx >= mrb->c->ciend - mrb->c->cibase)
    ciidx = 10; /* ciidx is broken... */

  for (i=ciidx; i >= 0; i--) {
    struct backtrace_location loc;
    mrb_callinfo *ci;
    mrb_irep *irep;
    const mrb_code *pc;

    ci = &mrb->c->cibase[i];

    if (!ci->proc) continue;
    if (MRB_PROC_CFUNC_P(ci->proc)) continue;

    irep = ci->proc->body.irep;
    if (!irep) continue;

    if (mrb->c->cibase[i].err) {
      pc = mrb->c->cibase[i].err;
    }
    else if (i+1 <= ciidx) {
      if (!mrb->c->cibase[i + 1].pc) continue;
      pc = &mrb->c->cibase[i+1].pc[-1];
    }
    else {
      pc = pc0;
    }

    loc.lineno = mrb_debug_get_line(mrb, irep, pc - irep->iseq);
    if (loc.lineno == -1) continue;

    loc.filename = mrb_debug_get_filename(mrb, irep, pc - irep->iseq);
    if (!loc.filename) {
      loc.filename = "(unknown)";
    }

    loc.method_id = ci->mid;
    func(mrb, &loc, data);
  }
}

#ifndef MRB_DISABLE_STDIO

static void
print_backtrace(mrb_state *mrb, struct RObject *exc, mrb_value backtrace)
{
  mrb_int i;
  mrb_int n = RARRAY_LEN(backtrace);
  mrb_value *loc, mesg;
  FILE *stream = stderr;

  if (n != 0) {
    fprintf(stream, "trace (most recent call last):\n");
    for (i=n-1,loc=&RARRAY_PTR(backtrace)[i]; i>0; i--,loc--) {
      if (mrb_string_p(*loc)) {
        fprintf(stream, "\t[%d] %.*s\n",
                (int)i, (int)RSTRING_LEN(*loc), RSTRING_PTR(*loc));
      }
    }
    if (mrb_string_p(*loc)) {
      fprintf(stream, "%.*s: ", (int)RSTRING_LEN(*loc), RSTRING_PTR(*loc));
    }
  }
  mesg = mrb_exc_inspect(mrb, mrb_obj_value(exc));
  fprintf(stream, "%.*s\n", (int)RSTRING_LEN(mesg), RSTRING_PTR(mesg));
}

/* mrb_print_backtrace

   function to retrieve backtrace information from the last exception.
*/

MRB_API void
mrb_print_backtrace(mrb_state *mrb)
{
  mrb_value backtrace;

  if (!mrb->exc) {
    return;
  }

  backtrace = mrb_obj_iv_get(mrb, mrb->exc, mrb_intern_lit(mrb, "backtrace"));
  if (mrb_nil_p(backtrace)) return;
  if (!mrb_array_p(backtrace)) backtrace = mrb_unpack_backtrace(mrb, backtrace);
  print_backtrace(mrb, mrb->exc, backtrace);
}
#else

MRB_API void
mrb_print_backtrace(mrb_state *mrb)
{
}

#endif

static void
count_backtrace_i(mrb_state *mrb,
                 const struct backtrace_location *loc,
                 void *data)
{
  int *lenp = (int*)data;

  (*lenp)++;
}

static void
pack_backtrace_i(mrb_state *mrb,
                 const struct backtrace_location *loc,
                 void *data)
{
  struct backtrace_location **pptr = (struct backtrace_location**)data;
  struct backtrace_location *ptr = *pptr;

  *ptr = *loc;
  *pptr = ptr+1;
}

static mrb_value
packed_backtrace(mrb_state *mrb)
{
  struct RData *backtrace;
  ptrdiff_t ciidx = mrb->c->ci - mrb->c->cibase;
  int len = 0;
  int size;
  void *ptr;

  each_backtrace(mrb, ciidx, mrb->c->ci->pc, count_backtrace_i, &len);
  size = len * sizeof(struct backtrace_location);
  ptr = mrb_malloc(mrb, size);
  backtrace = mrb_data_object_alloc(mrb, NULL, ptr, &bt_type);
  backtrace->flags = (uint32_t)len;
  each_backtrace(mrb, ciidx, mrb->c->ci->pc, pack_backtrace_i, &ptr);
  return mrb_obj_value(backtrace);
}

void
mrb_keep_backtrace(mrb_state *mrb, mrb_value exc)
{
  mrb_sym sym = mrb_intern_lit(mrb, "backtrace");
  mrb_value backtrace;
  int ai;

  if (mrb_iv_defined(mrb, exc, sym)) return;
  ai = mrb_gc_arena_save(mrb);
  backtrace = packed_backtrace(mrb);
  mrb_iv_set(mrb, exc, sym, backtrace);
  mrb_gc_arena_restore(mrb, ai);
}

mrb_value
mrb_unpack_backtrace(mrb_state *mrb, mrb_value backtrace)
{
  const struct backtrace_location *bt;
  mrb_int n, i;
  int ai;

  if (mrb_nil_p(backtrace)) {
  empty_backtrace:
    return mrb_ary_new_capa(mrb, 0);
  }
  if (mrb_array_p(backtrace)) return backtrace;
  bt = (struct backtrace_location*)mrb_data_check_get_ptr(mrb, backtrace, &bt_type);
  if (bt == NULL) goto empty_backtrace;
  n = (mrb_int)RDATA(backtrace)->flags;
  backtrace = mrb_ary_new_capa(mrb, n);
  ai = mrb_gc_arena_save(mrb);
  for (i = 0; i < n; i++) {
    const struct backtrace_location *entry = &bt[i];
    mrb_value btline;

    btline = mrb_format(mrb, "%s:%d", entry->filename, (int)entry->lineno);
    if (entry->method_id != 0) {
      mrb_str_cat_lit(mrb, btline, ":in ");
      mrb_str_cat_cstr(mrb, btline, mrb_sym_name(mrb, entry->method_id));
    }
    mrb_ary_push(mrb, backtrace, btline);
    mrb_gc_arena_restore(mrb, ai);
  }

  return backtrace;
}

MRB_API mrb_value
mrb_exc_backtrace(mrb_state *mrb, mrb_value exc)
{
  mrb_sym attr_name;
  mrb_value backtrace;

  attr_name = mrb_intern_lit(mrb, "backtrace");
  backtrace = mrb_iv_get(mrb, exc, attr_name);
  if (mrb_nil_p(backtrace) || mrb_array_p(backtrace)) {
    return backtrace;
  }
  backtrace = mrb_unpack_backtrace(mrb, backtrace);
  mrb_iv_set(mrb, exc, attr_name, backtrace);
  return backtrace;
}

MRB_API mrb_value
mrb_get_backtrace(mrb_state *mrb)
{
  return mrb_unpack_backtrace(mrb, packed_backtrace(mrb));
}
/*
** class.c - Class class
**
** See Copyright Notice in mruby.h
*/

#include <stdarg.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/class.h>
#include <mruby/numeric.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/data.h>
#include <mruby/istruct.h>
#include <mruby/opcode.h>

KHASH_DEFINE(mt, mrb_sym, mrb_method_t, TRUE, kh_int_hash_func, kh_int_hash_equal)

void
mrb_gc_mark_mt(mrb_state *mrb, struct RClass *c)
{
  khiter_t k;
  khash_t(mt) *h = c->mt;

  if (!h) return;
  for (k = kh_begin(h); k != kh_end(h); k++) {
    if (kh_exist(h, k)) {
      mrb_method_t m = kh_value(h, k);

      if (MRB_METHOD_PROC_P(m)) {
        struct RProc *p = MRB_METHOD_PROC(m);
        mrb_gc_mark(mrb, (struct RBasic*)p);
      }
    }
  }
}

size_t
mrb_gc_mark_mt_size(mrb_state *mrb, struct RClass *c)
{
  khash_t(mt) *h = c->mt;

  if (!h) return 0;
  return kh_size(h);
}

void
mrb_gc_free_mt(mrb_state *mrb, struct RClass *c)
{
  kh_destroy(mt, mrb, c->mt);
}

void
mrb_class_name_class(mrb_state *mrb, struct RClass *outer, struct RClass *c, mrb_sym id)
{
  mrb_value name;
  mrb_sym nsym = mrb_intern_lit(mrb, "__classname__");

  if (mrb_obj_iv_defined(mrb, (struct RObject*)c, nsym)) return;
  if (outer == NULL || outer == mrb->object_class) {
    name = mrb_symbol_value(id);
  }
  else {
    name = mrb_class_path(mrb, outer);
    if (mrb_nil_p(name)) {      /* unnamed outer class */
      if (outer != mrb->object_class && outer != c) {
        mrb_obj_iv_set_force(mrb, (struct RObject*)c, mrb_intern_lit(mrb, "__outer__"),
                             mrb_obj_value(outer));
      }
      return;
    }
    else {
      mrb_int len;
      const char *n = mrb_sym_name_len(mrb, id, &len);

      mrb_str_cat_lit(mrb, name, "::");
      mrb_str_cat(mrb, name, n, len);
    }
  }
  mrb_obj_iv_set_force(mrb, (struct RObject*)c, nsym, name);
}

mrb_bool
mrb_const_name_p(mrb_state *mrb, const char *name, mrb_int len)
{
  return len > 0 && ISUPPER(name[0]) && mrb_ident_p(name+1, len-1);
}

static void
setup_class(mrb_state *mrb, struct RClass *outer, struct RClass *c, mrb_sym id)
{
  mrb_class_name_class(mrb, outer, c, id);
  mrb_obj_iv_set(mrb, (struct RObject*)outer, id, mrb_obj_value(c));
}

#define make_metaclass(mrb, c) prepare_singleton_class((mrb), (struct RBasic*)(c))

static void
prepare_singleton_class(mrb_state *mrb, struct RBasic *o)
{
  struct RClass *sc, *c;

  if (o->c->tt == MRB_TT_SCLASS) return;
  sc = (struct RClass*)mrb_obj_alloc(mrb, MRB_TT_SCLASS, mrb->class_class);
  sc->flags |= MRB_FL_CLASS_IS_INHERITED;
  sc->mt = kh_init(mt, mrb);
  sc->iv = 0;
  if (o->tt == MRB_TT_CLASS) {
    c = (struct RClass*)o;
    if (!c->super) {
      sc->super = mrb->class_class;
    }
    else {
      sc->super = c->super->c;
    }
  }
  else if (o->tt == MRB_TT_SCLASS) {
    c = (struct RClass*)o;
    while (c->super->tt == MRB_TT_ICLASS)
      c = c->super;
    make_metaclass(mrb, c->super);
    sc->super = c->super->c;
  }
  else {
    sc->super = o->c;
    prepare_singleton_class(mrb, (struct RBasic*)sc);
  }
  o->c = sc;
  mrb_field_write_barrier(mrb, (struct RBasic*)o, (struct RBasic*)sc);
  mrb_field_write_barrier(mrb, (struct RBasic*)sc, (struct RBasic*)o);
  mrb_obj_iv_set(mrb, (struct RObject*)sc, mrb_intern_lit(mrb, "__attached__"), mrb_obj_value(o));
  sc->flags |= o->flags & MRB_FL_OBJ_IS_FROZEN;
}

static mrb_value
class_name_str(mrb_state *mrb, struct RClass* c)
{
  mrb_value path = mrb_class_path(mrb, c);
  if (mrb_nil_p(path)) {
    path = c->tt == MRB_TT_MODULE ? mrb_str_new_lit(mrb, "#<Module:") :
                                    mrb_str_new_lit(mrb, "#<Class:");
    mrb_str_cat_str(mrb, path, mrb_ptr_to_str(mrb, c));
    mrb_str_cat_lit(mrb, path, ">");
  }
  return path;
}

static struct RClass*
class_from_sym(mrb_state *mrb, struct RClass *klass, mrb_sym id)
{
  mrb_value c = mrb_const_get(mrb, mrb_obj_value(klass), id);

  mrb_check_type(mrb, c, MRB_TT_CLASS);
  return mrb_class_ptr(c);
}

static struct RClass*
module_from_sym(mrb_state *mrb, struct RClass *klass, mrb_sym id)
{
  mrb_value c = mrb_const_get(mrb, mrb_obj_value(klass), id);

  mrb_check_type(mrb, c, MRB_TT_MODULE);
  return mrb_class_ptr(c);
}

static mrb_bool
class_ptr_p(mrb_value obj)
{
  switch (mrb_type(obj)) {
  case MRB_TT_CLASS:
  case MRB_TT_SCLASS:
  case MRB_TT_MODULE:
    return TRUE;
  default:
    return FALSE;
  }
}

static void
check_if_class_or_module(mrb_state *mrb, mrb_value obj)
{
  if (!class_ptr_p(obj)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "%!v is not a class/module", obj);
  }
}

static struct RClass*
define_module(mrb_state *mrb, mrb_sym name, struct RClass *outer)
{
  struct RClass *m;

  if (mrb_const_defined_at(mrb, mrb_obj_value(outer), name)) {
    return module_from_sym(mrb, outer, name);
  }
  m = mrb_module_new(mrb);
  setup_class(mrb, outer, m, name);

  return m;
}

MRB_API struct RClass*
mrb_define_module_id(mrb_state *mrb, mrb_sym name)
{
  return define_module(mrb, name, mrb->object_class);
}

MRB_API struct RClass*
mrb_define_module(mrb_state *mrb, const char *name)
{
  return define_module(mrb, mrb_intern_cstr(mrb, name), mrb->object_class);
}

struct RClass*
mrb_vm_define_module(mrb_state *mrb, mrb_value outer, mrb_sym id)
{
  check_if_class_or_module(mrb, outer);
  if (mrb_const_defined_at(mrb, outer, id)) {
    mrb_value old = mrb_const_get(mrb, outer, id);

    if (!mrb_module_p(old)) {
      mrb_raisef(mrb, E_TYPE_ERROR, "%!v is not a module", old);
    }
    return mrb_class_ptr(old);
  }
  return define_module(mrb, id, mrb_class_ptr(outer));
}

MRB_API struct RClass*
mrb_define_module_under(mrb_state *mrb, struct RClass *outer, const char *name)
{
  mrb_sym id = mrb_intern_cstr(mrb, name);
  struct RClass * c = define_module(mrb, id, outer);

  setup_class(mrb, outer, c, id);
  return c;
}

static struct RClass*
find_origin(struct RClass *c)
{
  MRB_CLASS_ORIGIN(c);
  return c;
}

static struct RClass*
define_class(mrb_state *mrb, mrb_sym name, struct RClass *super, struct RClass *outer)
{
  struct RClass * c;

  if (mrb_const_defined_at(mrb, mrb_obj_value(outer), name)) {
    c = class_from_sym(mrb, outer, name);
    MRB_CLASS_ORIGIN(c);
    if (super && mrb_class_real(c->super) != super) {
      mrb_raisef(mrb, E_TYPE_ERROR, "superclass mismatch for Class %n (%C not %C)",
                 name, c->super, super);
    }
    return c;
  }

  c = mrb_class_new(mrb, super);
  setup_class(mrb, outer, c, name);

  return c;
}

MRB_API struct RClass*
mrb_define_class_id(mrb_state *mrb, mrb_sym name, struct RClass *super)
{
  if (!super) {
    mrb_warn(mrb, "no super class for '%n', Object assumed", name);
  }
  return define_class(mrb, name, super, mrb->object_class);
}

MRB_API struct RClass*
mrb_define_class(mrb_state *mrb, const char *name, struct RClass *super)
{
  return mrb_define_class_id(mrb, mrb_intern_cstr(mrb, name), super);
}

static mrb_value mrb_bob_init(mrb_state *mrb, mrb_value);
#ifdef MRB_METHOD_CACHE
static void mc_clear_all(mrb_state *mrb);
static void mc_clear_by_id(mrb_state *mrb, struct RClass*, mrb_sym);
#else
#define mc_clear_all(mrb)
#define mc_clear_by_id(mrb,c,s)
#endif

static void
mrb_class_inherited(mrb_state *mrb, struct RClass *super, struct RClass *klass)
{
  mrb_value s;
  mrb_sym mid;

  if (!super)
    super = mrb->object_class;
  super->flags |= MRB_FL_CLASS_IS_INHERITED;
  s = mrb_obj_value(super);
  mrb_mc_clear_by_class(mrb, klass);
  mid = mrb_intern_lit(mrb, "inherited");
  if (!mrb_func_basic_p(mrb, s, mid, mrb_bob_init)) {
    mrb_value c = mrb_obj_value(klass);
    mrb_funcall_argv(mrb, s, mid, 1, &c);
  }
}

struct RClass*
mrb_vm_define_class(mrb_state *mrb, mrb_value outer, mrb_value super, mrb_sym id)
{
  struct RClass *s;
  struct RClass *c;

  if (!mrb_nil_p(super)) {
    if (!mrb_class_p(super)) {
      mrb_raisef(mrb, E_TYPE_ERROR, "superclass must be a Class (%!v given)", super);
    }
    s = mrb_class_ptr(super);
  }
  else {
    s = 0;
  }
  check_if_class_or_module(mrb, outer);
  if (mrb_const_defined_at(mrb, outer, id)) {
    mrb_value old = mrb_const_get(mrb, outer, id);

    if (!mrb_class_p(old)) {
      mrb_raisef(mrb, E_TYPE_ERROR, "%!v is not a class", old);
    }
    c = mrb_class_ptr(old);
    if (s) {
      /* check super class */
      if (mrb_class_real(c->super) != s) {
        mrb_raisef(mrb, E_TYPE_ERROR, "superclass mismatch for class %v", old);
      }
    }
    return c;
  }
  c = define_class(mrb, id, s, mrb_class_ptr(outer));
  mrb_class_inherited(mrb, mrb_class_real(c->super), c);

  return c;
}

MRB_API mrb_bool
mrb_class_defined(mrb_state *mrb, const char *name)
{
  mrb_value sym = mrb_check_intern_cstr(mrb, name);
  if (mrb_nil_p(sym)) {
    return FALSE;
  }
  return mrb_const_defined(mrb, mrb_obj_value(mrb->object_class), mrb_symbol(sym));
}

MRB_API mrb_bool
mrb_class_defined_under(mrb_state *mrb, struct RClass *outer, const char *name)
{
  mrb_value sym = mrb_check_intern_cstr(mrb, name);
  if (mrb_nil_p(sym)) {
    return FALSE;
  }
  return mrb_const_defined_at(mrb, mrb_obj_value(outer), mrb_symbol(sym));
}

MRB_API struct RClass*
mrb_class_get_under(mrb_state *mrb, struct RClass *outer, const char *name)
{
  return class_from_sym(mrb, outer, mrb_intern_cstr(mrb, name));
}

MRB_API struct RClass*
mrb_class_get(mrb_state *mrb, const char *name)
{
  return mrb_class_get_under(mrb, mrb->object_class, name);
}

MRB_API struct RClass*
mrb_exc_get(mrb_state *mrb, const char *name)
{
  struct RClass *exc, *e;
  mrb_value c = mrb_const_get(mrb, mrb_obj_value(mrb->object_class),
                              mrb_intern_cstr(mrb, name));

  if (!mrb_class_p(c)) {
    mrb_raise(mrb, mrb->eException_class, "exception corrupted");
  }
  exc = e = mrb_class_ptr(c);

  while (e) {
    if (e == mrb->eException_class)
      return exc;
    e = e->super;
  }
  return mrb->eException_class;
}

MRB_API struct RClass*
mrb_module_get_under(mrb_state *mrb, struct RClass *outer, const char *name)
{
  return module_from_sym(mrb, outer, mrb_intern_cstr(mrb, name));
}

MRB_API struct RClass*
mrb_module_get(mrb_state *mrb, const char *name)
{
  return mrb_module_get_under(mrb, mrb->object_class, name);
}

/*!
 * Defines a class under the namespace of \a outer.
 * \param outer  a class which contains the new class.
 * \param name     name of the new class
 * \param super  a class from which the new class will derive.
 *               NULL means \c Object class.
 * \return the created class
 * \throw TypeError if the constant name \a name is already taken but
 *                  the constant is not a \c Class.
 * \throw NameError if the class is already defined but the class can not
 *                  be reopened because its superclass is not \a super.
 * \post top-level constant named \a name refers the returned class.
 *
 * \note if a class named \a name is already defined and its superclass is
 *       \a super, the function just returns the defined class.
 */
MRB_API struct RClass*
mrb_define_class_under(mrb_state *mrb, struct RClass *outer, const char *name, struct RClass *super)
{
  mrb_sym id = mrb_intern_cstr(mrb, name);
  struct RClass * c;

#if 0
  if (!super) {
    mrb_warn(mrb, "no super class for '%C::%n', Object assumed", outer, id);
  }
#endif
  c = define_class(mrb, id, super, outer);
  setup_class(mrb, outer, c, id);
  return c;
}

MRB_API void
mrb_define_method_raw(mrb_state *mrb, struct RClass *c, mrb_sym mid, mrb_method_t m)
{
  khash_t(mt) *h;
  khiter_t k;
  MRB_CLASS_ORIGIN(c);
  h = c->mt;

  mrb_check_frozen(mrb, c);
  if (!h) h = c->mt = kh_init(mt, mrb);
  k = kh_put(mt, mrb, h, mid);
  kh_value(h, k) = m;
  if (MRB_METHOD_PROC_P(m) && !MRB_METHOD_UNDEF_P(m)) {
    struct RProc *p = MRB_METHOD_PROC(m);

    p->flags |= MRB_PROC_SCOPE;
    p->c = NULL;
    mrb_field_write_barrier(mrb, (struct RBasic*)c, (struct RBasic*)p);
    if (!MRB_PROC_ENV_P(p)) {
      MRB_PROC_SET_TARGET_CLASS(p, c);
    }
  }
  mc_clear_by_id(mrb, c, mid);
}

MRB_API void
mrb_define_method_id(mrb_state *mrb, struct RClass *c, mrb_sym mid, mrb_func_t func, mrb_aspec aspec)
{
  mrb_method_t m;
  int ai = mrb_gc_arena_save(mrb);

  MRB_METHOD_FROM_FUNC(m, func);
  if (aspec == MRB_ARGS_NONE()) {
    MRB_METHOD_NOARG_SET(m);
  }
  mrb_define_method_raw(mrb, c, mid, m);
  mrb_gc_arena_restore(mrb, ai);
}

MRB_API void
mrb_define_method(mrb_state *mrb, struct RClass *c, const char *name, mrb_func_t func, mrb_aspec aspec)
{
  mrb_define_method_id(mrb, c, mrb_intern_cstr(mrb, name), func, aspec);
}

/* a function to raise NotImplementedError with current method name */
MRB_API void
mrb_notimplement(mrb_state *mrb)
{
  mrb_callinfo *ci = mrb->c->ci;

  if (ci->mid) {
    mrb_raisef(mrb, E_NOTIMP_ERROR, "%n() function is unimplemented on this machine", ci->mid);
  }
}

/* a function to be replacement of unimplemented method */
MRB_API mrb_value
mrb_notimplement_m(mrb_state *mrb, mrb_value self)
{
  mrb_notimplement(mrb);
  /* not reached */
  return mrb_nil_value();
}

static mrb_value
to_ary(mrb_state *mrb, mrb_value val)
{
  mrb_check_type(mrb, val, MRB_TT_ARRAY);
  return val;
}

static mrb_value
to_hash(mrb_state *mrb, mrb_value val)
{
  mrb_check_type(mrb, val, MRB_TT_HASH);
  return val;
}

#define to_sym(mrb, ss) mrb_obj_to_sym(mrb, ss)

MRB_API mrb_int
mrb_get_argc(mrb_state *mrb)
{
  mrb_int argc = mrb->c->ci->argc;

  if (argc < 0) {
    struct RArray *a = mrb_ary_ptr(mrb->c->stack[1]);

    argc = ARY_LEN(a);
  }
  return argc;
}

MRB_API mrb_value*
mrb_get_argv(mrb_state *mrb)
{
  mrb_int argc = mrb->c->ci->argc;
  mrb_value *array_argv = mrb->c->stack + 1;
  if (argc < 0) {
    struct RArray *a = mrb_ary_ptr(*array_argv);

    array_argv = ARY_PTR(a);
  }
  return array_argv;
}

MRB_API mrb_value
mrb_get_arg1(mrb_state *mrb)
{
  mrb_int argc = mrb->c->ci->argc;
  mrb_value *array_argv = mrb->c->stack + 1;
  if (argc < 0) {
    struct RArray *a = mrb_ary_ptr(*array_argv);

    argc = ARY_LEN(a);
    array_argv = ARY_PTR(a);
  }
  if (argc != 1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
  }
  return array_argv[0];
}

void mrb_hash_check_kdict(mrb_state *mrb, mrb_value self);

/*
  retrieve arguments from mrb_state.

  mrb_get_args(mrb, format, ...)

  returns number of arguments parsed.

  format specifiers:

    string  mruby type     C type                 note
    ----------------------------------------------------------------------------------------------
    o:      Object         [mrb_value]
    C:      Class/Module   [mrb_value]
    S:      String         [mrb_value]            when ! follows, the value may be nil
    A:      Array          [mrb_value]            when ! follows, the value may be nil
    H:      Hash           [mrb_value]            when ! follows, the value may be nil
    s:      String         [char*,mrb_int]        Receive two arguments; s! gives (NULL,0) for nil
    z:      String         [char*]                NUL terminated string; z! gives NULL for nil
    a:      Array          [mrb_value*,mrb_int]   Receive two arguments; a! gives (NULL,0) for nil
    f:      Fixnum/Float   [mrb_float]
    i:      Fixnum/Float   [mrb_int]
    b:      boolean        [mrb_bool]
    n:      String/Symbol  [mrb_sym]
    d:      data           [void*,mrb_data_type const] 2nd argument will be used to check data type so it won't be modified; when ! follows, the value may be nil
    I:      inline struct  [void*]
    &:      block          [mrb_value]            &! raises exception if no block given
    *:      rest argument  [mrb_value*,mrb_int]   The rest of the arguments as an array; *! avoid copy of the stack
    |:      optional                              Following arguments are optional
    ?:      optional given [mrb_bool]             true if preceding argument (optional) is given
    ':':    keyword args   [mrb_kwargs const]     Get keyword arguments
 */
MRB_API mrb_int
mrb_get_args(mrb_state *mrb, const char *format, ...)
{
  const char *fmt = format;
  char c;
  mrb_int i = 0;
  va_list ap;
  mrb_int argc = mrb->c->ci->argc;
  mrb_value *array_argv = mrb->c->stack+1;
  mrb_bool argv_on_stack = argc >= 0;
  mrb_bool opt = FALSE;
  mrb_bool opt_skip = TRUE;
  mrb_bool given = TRUE;
  mrb_value kdict;
  mrb_bool reqkarg = FALSE;
  mrb_int needargc = 0;

  if (!argv_on_stack) {
    struct RArray *a = mrb_ary_ptr(*array_argv);
    array_argv = ARY_PTR(a);
    argc = ARY_LEN(a);
  }
  va_start(ap, format);

#define ARGV array_argv

  while ((c = *fmt++)) {
    switch (c) {
    case '|':
      opt = TRUE;
      break;
    case '*':
      opt_skip = FALSE;
      if (!reqkarg) reqkarg = strchr(fmt, ':') ? TRUE : FALSE;
      goto check_exit;
    case '!':
      break;
    case ':':
      reqkarg = TRUE;
      /* fall through */
    case '&': case '?':
      if (opt) opt_skip = FALSE;
      break;
    default:
      if (!opt) needargc ++;
      break;
    }
  }

 check_exit:
  if (reqkarg && argc > needargc && mrb_hash_p(kdict = ARGV[argc - 1])) {
    mrb_hash_check_kdict(mrb, kdict);
    argc --;
  }
  else {
    kdict = mrb_nil_value();
  }

  opt = FALSE;
  i = 0;
  while ((c = *format++)) {
    mrb_value *argv = ARGV;
    mrb_bool altmode;

    switch (c) {
    case '|': case '*': case '&': case '?': case ':':
      break;
    default:
      if (argc <= i) {
        if (opt) {
          given = FALSE;
        }
        else {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
        }
      }
      break;
    }

    if (*format == '!') {
      format ++;
      altmode = TRUE;
    }
    else {
      altmode = FALSE;
    }

    switch (c) {
    case 'o':
      {
        mrb_value *p;

        p = va_arg(ap, mrb_value*);
        if (i < argc) {
          *p = argv[i++];
        }
      }
      break;
    case 'C':
      {
        mrb_value *p;

        p = va_arg(ap, mrb_value*);
        if (i < argc) {
          mrb_value ss;

          ss = argv[i++];
          if (!class_ptr_p(ss)) {
            mrb_raisef(mrb, E_TYPE_ERROR, "%v is not class/module", ss);
          }
          *p = ss;
        }
      }
      break;
    case 'S':
      {
        mrb_value *p;

        p = va_arg(ap, mrb_value*);
        if (i < argc) {
          *p = argv[i++];
          if (!(altmode && mrb_nil_p(*p))) {
            mrb_to_str(mrb, *p);
          }
        }
      }
      break;
    case 'A':
      {
        mrb_value *p;

        p = va_arg(ap, mrb_value*);
        if (i < argc) {
          *p = argv[i++];
          if (!(altmode && mrb_nil_p(*p))) {
            *p = to_ary(mrb, *p);
          }
        }
      }
      break;
    case 'H':
      {
        mrb_value *p;

        p = va_arg(ap, mrb_value*);
        if (i < argc) {
          *p = argv[i++];
          if (!(altmode && mrb_nil_p(*p))) {
            *p = to_hash(mrb, *p);
          }
        }
      }
      break;
    case 's':
      {
        mrb_value ss;
        char **ps = 0;
        mrb_int *pl = 0;

        ps = va_arg(ap, char**);
        pl = va_arg(ap, mrb_int*);
        if (i < argc) {
          ss = argv[i++];
          if (altmode && mrb_nil_p(ss)) {
            *ps = NULL;
            *pl = 0;
          }
          else {
            mrb_to_str(mrb, ss);
            *ps = RSTRING_PTR(ss);
            *pl = RSTRING_LEN(ss);
          }
        }
      }
      break;
    case 'z':
      {
        mrb_value ss;
        const char **ps;

        ps = va_arg(ap, const char**);
        if (i < argc) {
          ss = argv[i++];
          if (altmode && mrb_nil_p(ss)) {
            *ps = NULL;
          }
          else {
            mrb_to_str(mrb, ss);
            *ps = RSTRING_CSTR(mrb, ss);
          }
        }
      }
      break;
    case 'a':
      {
        mrb_value aa;
        struct RArray *a;
        mrb_value **pb;
        mrb_int *pl;

        pb = va_arg(ap, mrb_value**);
        pl = va_arg(ap, mrb_int*);
        if (i < argc) {
          aa = argv[i++];
          if (altmode && mrb_nil_p(aa)) {
            *pb = 0;
            *pl = 0;
          }
          else {
            aa = to_ary(mrb, aa);
            a = mrb_ary_ptr(aa);
            *pb = ARY_PTR(a);
            *pl = ARY_LEN(a);
          }
        }
      }
      break;
    case 'I':
      {
        void* *p;
        mrb_value ss;

        p = va_arg(ap, void**);
        if (i < argc) {
          ss = argv[i++];
          if (!mrb_istruct_p(ss))
          {
            mrb_raisef(mrb, E_TYPE_ERROR, "%v is not inline struct", ss);
          }
          *p = mrb_istruct_ptr(ss);
        }
      }
      break;
#ifndef MRB_WITHOUT_FLOAT
    case 'f':
      {
        mrb_float *p;

        p = va_arg(ap, mrb_float*);
        if (i < argc) {
          *p = mrb_to_flo(mrb, argv[i++]);
        }
      }
      break;
#endif
    case 'i':
      {
        mrb_int *p;

        p = va_arg(ap, mrb_int*);
        if (i < argc) {
          *p = mrb_fixnum(mrb_to_int(mrb, argv[i++]));
        }
      }
      break;
    case 'b':
      {
        mrb_bool *boolp = va_arg(ap, mrb_bool*);

        if (i < argc) {
          mrb_value b = argv[i++];
          *boolp = mrb_test(b);
        }
      }
      break;
    case 'n':
      {
        mrb_sym *symp;

        symp = va_arg(ap, mrb_sym*);
        if (i < argc) {
          mrb_value ss;

          ss = argv[i++];
          *symp = to_sym(mrb, ss);
        }
      }
      break;
    case 'd':
      {
        void** datap;
        struct mrb_data_type const* type;

        datap = va_arg(ap, void**);
        type = va_arg(ap, struct mrb_data_type const*);
        if (i < argc) {
          mrb_value dd = argv[i++];
          if (altmode && mrb_nil_p(dd)) {
            *datap = 0;
          }
          else {
            *datap = mrb_data_get_ptr(mrb, dd, type);
          }
        }
      }
      break;

    case '&':
      {
        mrb_value *p, *bp;

        p = va_arg(ap, mrb_value*);
        if (mrb->c->ci->argc < 0) {
          bp = mrb->c->stack + 2;
        }
        else {
          bp = mrb->c->stack + mrb->c->ci->argc + 1;
        }
        if (altmode && mrb_nil_p(*bp)) {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
        }
        *p = *bp;
      }
      break;
    case '|':
      if (opt_skip && i == argc) goto finish;
      opt = TRUE;
      break;
    case '?':
      {
        mrb_bool *p;

        p = va_arg(ap, mrb_bool*);
        *p = given;
      }
      break;

    case '*':
      {
        mrb_value **var;
        mrb_int *pl;
        mrb_bool nocopy = (altmode || !argv_on_stack) ? TRUE : FALSE;

        var = va_arg(ap, mrb_value**);
        pl = va_arg(ap, mrb_int*);
        if (argc > i) {
          *pl = argc-i;
          if (*pl > 0) {
            if (nocopy) {
              *var = argv+i;
            }
            else {
              mrb_value args = mrb_ary_new_from_values(mrb, *pl, argv+i);
              RARRAY(args)->c = NULL;
              *var = RARRAY_PTR(args);
            }
          }
          i = argc;
        }
        else {
          *pl = 0;
          *var = NULL;
        }
      }
      break;

    case ':':
      {
        mrb_value ksrc = mrb_hash_p(kdict) ? mrb_hash_dup(mrb, kdict) : mrb_hash_new(mrb);
        const mrb_kwargs *kwargs = va_arg(ap, const mrb_kwargs*);
        mrb_value *rest;

        if (kwargs == NULL) {
          rest = NULL;
        }
        else {
          uint32_t kwnum = kwargs->num;
          uint32_t required = kwargs->required;
          const char *const *kname = kwargs->table;
          mrb_value *values = kwargs->values;
          uint32_t j;
          const uint32_t keyword_max = 40;

          if (kwnum > keyword_max || required > kwnum) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "keyword number is too large");
          }

          for (j = required; j > 0; j --, kname ++, values ++) {
            mrb_value k = mrb_symbol_value(mrb_intern_cstr(mrb, *kname));
            if (!mrb_hash_key_p(mrb, ksrc, k)) {
              mrb_raisef(mrb, E_ARGUMENT_ERROR, "missing keyword: %s", *kname);
            }
            *values = mrb_hash_delete_key(mrb, ksrc, k);
            mrb_gc_protect(mrb, *values);
          }

          for (j = kwnum - required; j > 0; j --, kname ++, values ++) {
            mrb_value k = mrb_symbol_value(mrb_intern_cstr(mrb, *kname));
            if (mrb_hash_key_p(mrb, ksrc, k)) {
              *values = mrb_hash_delete_key(mrb, ksrc, k);
              mrb_gc_protect(mrb, *values);
            }
            else {
              *values = mrb_undef_value();
            }
          }

          rest = kwargs->rest;
        }

        if (rest) {
          *rest = ksrc;
        }
        else if (!mrb_hash_empty_p(mrb, ksrc)) {
          ksrc = mrb_hash_keys(mrb, ksrc);
          ksrc = RARRAY_PTR(ksrc)[0];
          mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown keyword: %v", ksrc);
        }
      }
      break;

    default:
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid argument specifier %c", c);
      break;
    }
  }

#undef ARGV

  if (!c && argc > i) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
  }

finish:
  va_end(ap);
  return i;
}

static struct RClass*
boot_defclass(mrb_state *mrb, struct RClass *super)
{
  struct RClass *c;

  c = (struct RClass*)mrb_obj_alloc(mrb, MRB_TT_CLASS, mrb->class_class);
  if (super) {
    c->super = super;
    mrb_field_write_barrier(mrb, (struct RBasic*)c, (struct RBasic*)super);
  }
  else {
    c->super = mrb->object_class;
  }
  c->mt = kh_init(mt, mrb);
  return c;
}

static void
boot_initmod(mrb_state *mrb, struct RClass *mod)
{
  if (!mod->mt) {
    mod->mt = kh_init(mt, mrb);
  }
}

static struct RClass*
include_class_new(mrb_state *mrb, struct RClass *m, struct RClass *super)
{
  struct RClass *ic = (struct RClass*)mrb_obj_alloc(mrb, MRB_TT_ICLASS, mrb->class_class);
  if (m->tt == MRB_TT_ICLASS) {
    m = m->c;
  }
  MRB_CLASS_ORIGIN(m);
  ic->iv = m->iv;
  ic->mt = m->mt;
  ic->super = super;
  if (m->tt == MRB_TT_ICLASS) {
    ic->c = m->c;
  }
  else {
    ic->c = m;
  }
  return ic;
}

static int
include_module_at(mrb_state *mrb, struct RClass *c, struct RClass *ins_pos, struct RClass *m, int search_super)
{
  struct RClass *p, *ic;
  void *klass_mt = find_origin(c)->mt;

  while (m) {
    int superclass_seen = 0;

    if (m->flags & MRB_FL_CLASS_IS_PREPENDED)
      goto skip;

    if (klass_mt && klass_mt == m->mt)
      return -1;

    p = c->super;
    while (p) {
      if (p->tt == MRB_TT_ICLASS) {
        if (p->mt == m->mt) {
          if (!superclass_seen) {
            ins_pos = p; /* move insert point */
          }
          goto skip;
        }
      } else if (p->tt == MRB_TT_CLASS) {
        if (!search_super) break;
        superclass_seen = 1;
      }
      p = p->super;
    }

    ic = include_class_new(mrb, m, ins_pos->super);
    m->flags |= MRB_FL_CLASS_IS_INHERITED;
    ins_pos->super = ic;
    mrb_field_write_barrier(mrb, (struct RBasic*)ins_pos, (struct RBasic*)ic);
    mrb_mc_clear_by_class(mrb, ins_pos);
    ins_pos = ic;
  skip:
    m = m->super;
  }
  mc_clear_all(mrb);
  return 0;
}

MRB_API void
mrb_include_module(mrb_state *mrb, struct RClass *c, struct RClass *m)
{
  mrb_check_frozen(mrb, c);
  if (include_module_at(mrb, c, find_origin(c), m, 1) < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "cyclic include detected");
  }
}

MRB_API void
mrb_prepend_module(mrb_state *mrb, struct RClass *c, struct RClass *m)
{
  struct RClass *origin;
  int changed = 0;

  mrb_check_frozen(mrb, c);
  if (!(c->flags & MRB_FL_CLASS_IS_PREPENDED)) {
    origin = (struct RClass*)mrb_obj_alloc(mrb, MRB_TT_ICLASS, c);
    origin->flags |= MRB_FL_CLASS_IS_ORIGIN | MRB_FL_CLASS_IS_INHERITED;
    origin->super = c->super;
    c->super = origin;
    origin->mt = c->mt;
    c->mt = kh_init(mt, mrb);
    mrb_field_write_barrier(mrb, (struct RBasic*)c, (struct RBasic*)origin);
    c->flags |= MRB_FL_CLASS_IS_PREPENDED;
  }
  changed = include_module_at(mrb, c, c, m, 0);
  if (changed < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "cyclic prepend detected");
  }
}

static mrb_value
mrb_mod_prepend_features(mrb_state *mrb, mrb_value mod)
{
  mrb_value klass;

  mrb_check_type(mrb, mod, MRB_TT_MODULE);
  mrb_get_args(mrb, "C", &klass);
  mrb_prepend_module(mrb, mrb_class_ptr(klass), mrb_class_ptr(mod));
  return mod;
}

static mrb_value
mrb_mod_append_features(mrb_state *mrb, mrb_value mod)
{
  mrb_value klass;

  mrb_check_type(mrb, mod, MRB_TT_MODULE);
  mrb_get_args(mrb, "C", &klass);
  mrb_include_module(mrb, mrb_class_ptr(klass), mrb_class_ptr(mod));
  return mod;
}

/* 15.2.2.4.28 */
/*
 *  call-seq:
 *     mod.include?(module)    -> true or false
 *
 *  Returns <code>true</code> if <i>module</i> is included in
 *  <i>mod</i> or one of <i>mod</i>'s ancestors.
 *
 *     module A
 *     end
 *     class B
 *       include A
 *     end
 *     class C < B
 *     end
 *     B.include?(A)   #=> true
 *     C.include?(A)   #=> true
 *     A.include?(A)   #=> false
 */
static mrb_value
mrb_mod_include_p(mrb_state *mrb, mrb_value mod)
{
  mrb_value mod2;
  struct RClass *c = mrb_class_ptr(mod);

  mrb_get_args(mrb, "C", &mod2);
  mrb_check_type(mrb, mod2, MRB_TT_MODULE);

  while (c) {
    if (c->tt == MRB_TT_ICLASS) {
      if (c->c == mrb_class_ptr(mod2)) return mrb_true_value();
    }
    c = c->super;
  }
  return mrb_false_value();
}

static mrb_value
mrb_mod_ancestors(mrb_state *mrb, mrb_value self)
{
  mrb_value result;
  struct RClass *c = mrb_class_ptr(self);
  result = mrb_ary_new(mrb);
  while (c) {
    if (c->tt == MRB_TT_ICLASS) {
      mrb_ary_push(mrb, result, mrb_obj_value(c->c));
    }
    else if (!(c->flags & MRB_FL_CLASS_IS_PREPENDED)) {
      mrb_ary_push(mrb, result, mrb_obj_value(c));
    }
    c = c->super;
  }

  return result;
}

static mrb_value
mrb_mod_extend_object(mrb_state *mrb, mrb_value mod)
{
  mrb_value obj = mrb_get_arg1(mrb);

  mrb_check_type(mrb, mod, MRB_TT_MODULE);
  mrb_include_module(mrb, mrb_class_ptr(mrb_singleton_class(mrb, obj)), mrb_class_ptr(mod));
  return mod;
}

static mrb_value
mrb_mod_initialize(mrb_state *mrb, mrb_value mod)
{
  mrb_value b;
  struct RClass *m = mrb_class_ptr(mod);
  boot_initmod(mrb, m); /* bootstrap a newly initialized module */
  mrb_get_args(mrb, "|&", &b);
  if (!mrb_nil_p(b)) {
    mrb_yield_with_class(mrb, b, 1, &mod, mod, m);
  }
  return mod;
}

/* implementation of module_eval/class_eval */
mrb_value mrb_mod_module_eval(mrb_state*, mrb_value);

static mrb_value
mrb_mod_dummy_visibility(mrb_state *mrb, mrb_value mod)
{
  return mod;
}

/* returns mrb_class_ptr(mrb_singleton_class()) */
/* except that it return NULL for immediate values */
MRB_API struct RClass*
mrb_singleton_class_ptr(mrb_state *mrb, mrb_value v)
{
  struct RBasic *obj;

  switch (mrb_type(v)) {
  case MRB_TT_FALSE:
    if (mrb_nil_p(v))
      return mrb->nil_class;
    return mrb->false_class;
  case MRB_TT_TRUE:
    return mrb->true_class;
  case MRB_TT_CPTR:
    return mrb->object_class;
  case MRB_TT_SYMBOL:
  case MRB_TT_FIXNUM:
#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
#endif
    return NULL;
  default:
    break;
  }
  obj = mrb_basic_ptr(v);
  prepare_singleton_class(mrb, obj);
  return obj->c;
}

MRB_API mrb_value
mrb_singleton_class(mrb_state *mrb, mrb_value v)
{
  struct RClass *c = mrb_singleton_class_ptr(mrb, v);

  if (c == NULL) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't define singleton");
  }
  return mrb_obj_value(c);
}

MRB_API void
mrb_define_singleton_method(mrb_state *mrb, struct RObject *o, const char *name, mrb_func_t func, mrb_aspec aspec)
{
  prepare_singleton_class(mrb, (struct RBasic*)o);
  mrb_define_method_id(mrb, o->c, mrb_intern_cstr(mrb, name), func, aspec);
}

MRB_API void
mrb_define_class_method(mrb_state *mrb, struct RClass *c, const char *name, mrb_func_t func, mrb_aspec aspec)
{
  mrb_define_singleton_method(mrb, (struct RObject*)c, name, func, aspec);
}

MRB_API void
mrb_define_module_function(mrb_state *mrb, struct RClass *c, const char *name, mrb_func_t func, mrb_aspec aspec)
{
  mrb_define_class_method(mrb, c, name, func, aspec);
  mrb_define_method(mrb, c, name, func, aspec);
}

#ifdef MRB_METHOD_CACHE
static void
mc_clear_all(mrb_state *mrb)
{
  struct mrb_cache_entry *mc = mrb->cache;
  int i;

  for (i=0; i<MRB_METHOD_CACHE_SIZE; i++) {
    mc[i].c = 0;
  }
}

void
mrb_mc_clear_by_class(mrb_state *mrb, struct RClass *c)
{
  struct mrb_cache_entry *mc = mrb->cache;
  int i;

  if (c->flags & MRB_FL_CLASS_IS_INHERITED) {
    mc_clear_all(mrb);
    c->flags &= ~MRB_FL_CLASS_IS_INHERITED;
    return;
  }
  for (i=0; i<MRB_METHOD_CACHE_SIZE; i++) {
    if (mc[i].c == c) mc[i].c = 0;
  }
}

static void
mc_clear_by_id(mrb_state *mrb, struct RClass *c, mrb_sym mid)
{
  struct mrb_cache_entry *mc = mrb->cache;
  int i;

  if (c->flags & MRB_FL_CLASS_IS_INHERITED) {
    mc_clear_all(mrb);
    c->flags &= ~MRB_FL_CLASS_IS_INHERITED;
    return;
  }
  for (i=0; i<MRB_METHOD_CACHE_SIZE; i++) {
    if (mc[i].c == c || mc[i].mid == mid)
      mc[i].c = 0;
  }
}
#endif

MRB_API mrb_method_t
mrb_method_search_vm(mrb_state *mrb, struct RClass **cp, mrb_sym mid)
{
  khiter_t k;
  mrb_method_t m;
  struct RClass *c = *cp;
#ifdef MRB_METHOD_CACHE
  struct RClass *oc = c;
  int h = kh_int_hash_func(mrb, ((intptr_t)oc) ^ mid) & (MRB_METHOD_CACHE_SIZE-1);
  struct mrb_cache_entry *mc = &mrb->cache[h];

  if (mc->c == c && mc->mid == mid) {
    *cp = mc->c0;
    return mc->m;
  }
#endif

  while (c) {
    khash_t(mt) *h = c->mt;

    if (h) {
      k = kh_get(mt, mrb, h, mid);
      if (k != kh_end(h)) {
        m = kh_value(h, k);
        if (MRB_METHOD_UNDEF_P(m)) break;
        *cp = c;
#ifdef MRB_METHOD_CACHE
        mc->c = oc;
        mc->c0 = c;
        mc->mid = mid;
        mc->m = m;
#endif
        return m;
      }
    }
    c = c->super;
  }
  MRB_METHOD_FROM_PROC(m, NULL);
  return m;                  /* no method */
}

MRB_API mrb_method_t
mrb_method_search(mrb_state *mrb, struct RClass* c, mrb_sym mid)
{
  mrb_method_t m;

  m = mrb_method_search_vm(mrb, &c, mid);
  if (MRB_METHOD_UNDEF_P(m)) {
    mrb_name_error(mrb, mid, "undefined method '%n' for class %C", mid, c);
  }
  return m;
}

#define ONSTACK_ALLOC_MAX 32

static mrb_sym
prepare_name_common(mrb_state *mrb, mrb_sym sym, const char *prefix, const char *suffix)
{
  char onstack[ONSTACK_ALLOC_MAX];
  mrb_int sym_len;
  const char *sym_str = mrb_sym_name_len(mrb, sym, &sym_len);
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  size_t suffix_len = suffix ? strlen(suffix) : 0;
  size_t name_len = sym_len + prefix_len + suffix_len;
  char *buf = name_len > sizeof(onstack) ? (char *)mrb_alloca(mrb, name_len) : onstack;
  char *p = buf;

  if (prefix_len > 0) {
    memcpy(p, prefix, prefix_len);
    p += prefix_len;
  }

  memcpy(p, sym_str, sym_len);
  p += sym_len;

  if (suffix_len > 0) {
    memcpy(p, suffix, suffix_len);
    p += suffix_len;
  }

  return mrb_intern(mrb, buf, name_len);
}

static mrb_value
prepare_ivar_name(mrb_state *mrb, mrb_sym sym)
{
  sym = prepare_name_common(mrb, sym, "@", NULL);
  mrb_iv_name_sym_check(mrb, sym);
  return mrb_symbol_value(sym);
}

static mrb_sym
prepare_writer_name(mrb_state *mrb, mrb_sym sym)
{
  return prepare_name_common(mrb, sym, NULL, "=");
}

static mrb_value
mod_attr_define(mrb_state *mrb, mrb_value mod, mrb_value (*accessor)(mrb_state *, mrb_value), mrb_sym (*access_name)(mrb_state *, mrb_sym))
{
  struct RClass *c = mrb_class_ptr(mod);
  mrb_value *argv;
  mrb_int argc, i;
  int ai;

  mrb_get_args(mrb, "*", &argv, &argc);
  ai = mrb_gc_arena_save(mrb);
  for (i=0; i<argc; i++) {
    mrb_value name;
    mrb_sym method;
    struct RProc *p;
    mrb_method_t m;

    method = to_sym(mrb, argv[i]);
    name = prepare_ivar_name(mrb, method);
    if (access_name) {
      method = access_name(mrb, method);
    }

    p = mrb_proc_new_cfunc_with_env(mrb, accessor, 1, &name);
    MRB_METHOD_FROM_PROC(m, p);
    mrb_define_method_raw(mrb, c, method, m);
    mrb_gc_arena_restore(mrb, ai);
  }
  return mrb_nil_value();
}

static mrb_value
attr_reader(mrb_state *mrb, mrb_value obj)
{
  mrb_value name = mrb_proc_cfunc_env_get(mrb, 0);
  return mrb_iv_get(mrb, obj, to_sym(mrb, name));
}

static mrb_value
mrb_mod_attr_reader(mrb_state *mrb, mrb_value mod)
{
  return mod_attr_define(mrb, mod, attr_reader, NULL);
}

static mrb_value
attr_writer(mrb_state *mrb, mrb_value obj)
{
  mrb_value name = mrb_proc_cfunc_env_get(mrb, 0);
  mrb_value val = mrb_get_arg1(mrb);

  mrb_iv_set(mrb, obj, to_sym(mrb, name), val);
  return val;
}

static mrb_value
mrb_mod_attr_writer(mrb_state *mrb, mrb_value mod)
{
  return mod_attr_define(mrb, mod, attr_writer, prepare_writer_name);
}

static mrb_value
mrb_instance_alloc(mrb_state *mrb, mrb_value cv)
{
  struct RClass *c = mrb_class_ptr(cv);
  struct RObject *o;
  enum mrb_vtype ttype = MRB_INSTANCE_TT(c);

  if (c->tt == MRB_TT_SCLASS)
    mrb_raise(mrb, E_TYPE_ERROR, "can't create instance of singleton class");

  if (ttype == 0) ttype = MRB_TT_OBJECT;
  if (ttype <= MRB_TT_CPTR) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't create instance of %v", cv);
  }
  o = (struct RObject*)mrb_obj_alloc(mrb, ttype, c);
  return mrb_obj_value(o);
}

/*
 *  call-seq:
 *     class.new(args, ...)    ->  obj
 *
 *  Creates a new object of <i>class</i>'s class, then
 *  invokes that object's <code>initialize</code> method,
 *  passing it <i>args</i>. This is the method that ends
 *  up getting called whenever an object is constructed using
 *  `.new`.
 *
 */

mrb_value
mrb_instance_new(mrb_state *mrb, mrb_value cv)
{
  mrb_value obj, blk;
  mrb_value *argv;
  mrb_int argc;
  mrb_sym init;

  mrb_get_args(mrb, "*!&", &argv, &argc, &blk);
  obj = mrb_instance_alloc(mrb, cv);
  init = mrb_intern_lit(mrb, "initialize");
  if (!mrb_func_basic_p(mrb, obj, init, mrb_bob_init)) {
    mrb_funcall_with_block(mrb, obj, init, argc, argv, blk);
  }
  return obj;
}

MRB_API mrb_value
mrb_obj_new(mrb_state *mrb, struct RClass *c, mrb_int argc, const mrb_value *argv)
{
  mrb_value obj;
  mrb_sym mid;

  obj = mrb_instance_alloc(mrb, mrb_obj_value(c));
  mid = mrb_intern_lit(mrb, "initialize");
  if (!mrb_func_basic_p(mrb, obj, mid, mrb_bob_init)) {
    mrb_funcall_argv(mrb, obj, mid, argc, argv);
  }
  return obj;
}

static mrb_value
mrb_class_initialize(mrb_state *mrb, mrb_value c)
{
  mrb_value a, b;

  mrb_get_args(mrb, "|C&", &a, &b);
  if (!mrb_nil_p(b)) {
    mrb_yield_with_class(mrb, b, 1, &c, c, mrb_class_ptr(c));
  }
  return c;
}

static mrb_value
mrb_class_new_class(mrb_state *mrb, mrb_value cv)
{
  mrb_int n;
  mrb_value super, blk;
  mrb_value new_class;
  mrb_sym mid;

  n = mrb_get_args(mrb, "|C&", &super, &blk);
  if (n == 0) {
    super = mrb_obj_value(mrb->object_class);
  }
  new_class = mrb_obj_value(mrb_class_new(mrb, mrb_class_ptr(super)));
  mid = mrb_intern_lit(mrb, "initialize");
  if (mrb_func_basic_p(mrb, new_class, mid, mrb_class_initialize)) {
    mrb_class_initialize(mrb, new_class);
  }
  else {
    mrb_funcall_with_block(mrb, new_class, mid, n, &super, blk);
  }
  mrb_class_inherited(mrb, mrb_class_ptr(super), mrb_class_ptr(new_class));
  return new_class;
}

static mrb_value
mrb_class_superclass(mrb_state *mrb, mrb_value klass)
{
  struct RClass *c;

  c = mrb_class_ptr(klass);
  c = find_origin(c)->super;
  while (c && c->tt == MRB_TT_ICLASS) {
    c = find_origin(c)->super;
  }
  if (!c) return mrb_nil_value();
  return mrb_obj_value(c);
}

static mrb_value
mrb_bob_init(mrb_state *mrb, mrb_value cv)
{
  return mrb_nil_value();
}

static mrb_value
mrb_bob_not(mrb_state *mrb, mrb_value cv)
{
  return mrb_bool_value(!mrb_test(cv));
}

/* 15.3.1.3.1  */
/* 15.3.1.3.10 */
/* 15.3.1.3.11 */
/*
 *  call-seq:
 *     obj == other        -> true or false
 *     obj.equal?(other)   -> true or false
 *     obj.eql?(other)     -> true or false
 *
 *  Equality---At the <code>Object</code> level, <code>==</code> returns
 *  <code>true</code> only if <i>obj</i> and <i>other</i> are the
 *  same object. Typically, this method is overridden in descendant
 *  classes to provide class-specific meaning.
 *
 *  Unlike <code>==</code>, the <code>equal?</code> method should never be
 *  overridden by subclasses: it is used to determine object identity
 *  (that is, <code>a.equal?(b)</code> iff <code>a</code> is the same
 *  object as <code>b</code>).
 *
 *  The <code>eql?</code> method returns <code>true</code> if
 *  <i>obj</i> and <i>anObject</i> have the same value. Used by
 *  <code>Hash</code> to test members for equality.  For objects of
 *  class <code>Object</code>, <code>eql?</code> is synonymous with
 *  <code>==</code>. Subclasses normally continue this tradition, but
 *  there are exceptions. <code>Numeric</code> types, for example,
 *  perform type conversion across <code>==</code>, but not across
 *  <code>eql?</code>, so:
 *
 *     1 == 1.0     #=> true
 *     1.eql? 1.0   #=> false
 */
mrb_value
mrb_obj_equal_m(mrb_state *mrb, mrb_value self)
{
  mrb_value arg = mrb_get_arg1(mrb);

  return mrb_bool_value(mrb_obj_equal(mrb, self, arg));
}

MRB_API mrb_bool
mrb_obj_respond_to(mrb_state *mrb, struct RClass* c, mrb_sym mid)
{
  mrb_method_t m;

  m = mrb_method_search_vm(mrb, &c, mid);
  if (MRB_METHOD_UNDEF_P(m)) {
    return FALSE;
  }
  return TRUE;
}

MRB_API mrb_bool
mrb_respond_to(mrb_state *mrb, mrb_value obj, mrb_sym mid)
{
  return mrb_obj_respond_to(mrb, mrb_class(mrb, obj), mid);
}

MRB_API mrb_value
mrb_class_path(mrb_state *mrb, struct RClass *c)
{
  mrb_value path;
  mrb_sym nsym = mrb_intern_lit(mrb, "__classname__");

  path = mrb_obj_iv_get(mrb, (struct RObject*)c, nsym);
  if (mrb_nil_p(path)) {
    /* no name (yet) */
    return mrb_class_find_path(mrb, c);
  }
  else if (mrb_symbol_p(path)) {
    /* toplevel class/module */
    return mrb_sym_str(mrb, mrb_symbol(path));
  }
  return mrb_str_dup(mrb, path);
}

MRB_API struct RClass*
mrb_class_real(struct RClass* cl)
{
  if (cl == 0) return NULL;
  while ((cl->tt == MRB_TT_SCLASS) || (cl->tt == MRB_TT_ICLASS)) {
    cl = cl->super;
    if (cl == 0) return NULL;
  }
  return cl;
}

MRB_API const char*
mrb_class_name(mrb_state *mrb, struct RClass* c)
{
  mrb_value name;

  if (c == NULL) return NULL;
  name = class_name_str(mrb, c);
  return RSTRING_PTR(name);
}

MRB_API const char*
mrb_obj_classname(mrb_state *mrb, mrb_value obj)
{
  return mrb_class_name(mrb, mrb_obj_class(mrb, obj));
}

/*!
 * Ensures a class can be derived from super.
 *
 * \param super a reference to an object.
 * \exception TypeError if \a super is not a Class or \a super is a singleton class.
 */
static void
mrb_check_inheritable(mrb_state *mrb, struct RClass *super)
{
  if (super->tt != MRB_TT_CLASS) {
    mrb_raisef(mrb, E_TYPE_ERROR, "superclass must be a Class (%C given)", super);
  }
  if (super->tt == MRB_TT_SCLASS) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't make subclass of singleton class");
  }
  if (super == mrb->class_class) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't make subclass of Class");
  }
}

/*!
 * Creates a new class.
 * \param super     a class from which the new class derives.
 * \exception TypeError \a super is not inheritable.
 * \exception TypeError \a super is the Class class.
 */
MRB_API struct RClass*
mrb_class_new(mrb_state *mrb, struct RClass *super)
{
  struct RClass *c;

  if (super) {
    mrb_check_inheritable(mrb, super);
  }
  c = boot_defclass(mrb, super);
  if (super) {
    MRB_SET_INSTANCE_TT(c, MRB_INSTANCE_TT(super));
  }
  make_metaclass(mrb, c);

  return c;
}

/*!
 * Creates a new module.
 */
MRB_API struct RClass*
mrb_module_new(mrb_state *mrb)
{
  struct RClass *m = (struct RClass*)mrb_obj_alloc(mrb, MRB_TT_MODULE, mrb->module_class);
  boot_initmod(mrb, m);
  return m;
}

/*
 *  call-seq:
 *     obj.class    => class
 *
 *  Returns the class of <i>obj</i>, now preferred over
 *  <code>Object#type</code>, as an object's type in Ruby is only
 *  loosely tied to that object's class. This method must always be
 *  called with an explicit receiver, as <code>class</code> is also a
 *  reserved word in Ruby.
 *
 *     1.class      #=> Fixnum
 *     self.class   #=> Object
 */

MRB_API struct RClass*
mrb_obj_class(mrb_state *mrb, mrb_value obj)
{
  return mrb_class_real(mrb_class(mrb, obj));
}

MRB_API void
mrb_alias_method(mrb_state *mrb, struct RClass *c, mrb_sym a, mrb_sym b)
{
  mrb_method_t m = mrb_method_search(mrb, c, b);

  if (!MRB_METHOD_CFUNC_P(m)) {
    struct RProc *p = MRB_METHOD_PROC(m);

    if (MRB_PROC_ENV_P(p)) {
      MRB_PROC_ENV(p)->mid = b;
    }
    else {
      struct RClass *tc = MRB_PROC_TARGET_CLASS(p);
      struct REnv *e = (struct REnv*)mrb_obj_alloc(mrb, MRB_TT_ENV, NULL);

      e->mid = b;
      if (tc) {
        e->c = tc;
        mrb_field_write_barrier(mrb, (struct RBasic*)e, (struct RBasic*)tc);
      }
      p->e.env = e;
      p->flags |= MRB_PROC_ENVSET;
    }
  }
  mrb_define_method_raw(mrb, c, a, m);
}

/*!
 * Defines an alias of a method.
 * \param mrb    the mruby state
 * \param klass  the class which the original method belongs to
 * \param name1  a new name for the method
 * \param name2  the original name of the method
 */
MRB_API void
mrb_define_alias(mrb_state *mrb, struct RClass *klass, const char *name1, const char *name2)
{
  mrb_alias_method(mrb, klass, mrb_intern_cstr(mrb, name1), mrb_intern_cstr(mrb, name2));
}

/*
 * call-seq:
 *   mod.to_s   -> string
 *
 * Return a string representing this module or class. For basic
 * classes and modules, this is the name. For singletons, we
 * show information on the thing we're attached to as well.
 */

mrb_value
mrb_mod_to_s(mrb_state *mrb, mrb_value klass)
{

  if (mrb_sclass_p(klass)) {
    mrb_value v = mrb_iv_get(mrb, klass, mrb_intern_lit(mrb, "__attached__"));
    mrb_value str = mrb_str_new_lit(mrb, "#<Class:");

    if (class_ptr_p(v)) {
      mrb_str_cat_str(mrb, str, mrb_inspect(mrb, v));
    }
    else {
      mrb_str_cat_str(mrb, str, mrb_any_to_s(mrb, v));
    }
    return mrb_str_cat_lit(mrb, str, ">");
  }
  else {
    return class_name_str(mrb, mrb_class_ptr(klass));
  }
}

static mrb_value
mrb_mod_alias(mrb_state *mrb, mrb_value mod)
{
  struct RClass *c = mrb_class_ptr(mod);
  mrb_sym new_name, old_name;

  mrb_get_args(mrb, "nn", &new_name, &old_name);
  mrb_alias_method(mrb, c, new_name, old_name);
  return mod;
}

static void
undef_method(mrb_state *mrb, struct RClass *c, mrb_sym a)
{
  mrb_method_t m;

  MRB_METHOD_FROM_PROC(m, NULL);
  mrb_define_method_raw(mrb, c, a, m);
}

void
mrb_undef_method_id(mrb_state *mrb, struct RClass *c, mrb_sym a)
{
  if (!mrb_obj_respond_to(mrb, c, a)) {
    mrb_name_error(mrb, a, "undefined method '%n' for class '%C'", a, c);
  }
  undef_method(mrb, c, a);
}

MRB_API void
mrb_undef_method(mrb_state *mrb, struct RClass *c, const char *name)
{
  undef_method(mrb, c, mrb_intern_cstr(mrb, name));
}

MRB_API void
mrb_undef_class_method(mrb_state *mrb, struct RClass *c, const char *name)
{
  mrb_undef_method(mrb,  mrb_class_ptr(mrb_singleton_class(mrb, mrb_obj_value(c))), name);
}

static mrb_value
mrb_mod_undef(mrb_state *mrb, mrb_value mod)
{
  struct RClass *c = mrb_class_ptr(mod);
  mrb_int argc;
  mrb_value *argv;

  mrb_get_args(mrb, "*", &argv, &argc);
  while (argc--) {
    mrb_undef_method_id(mrb, c, to_sym(mrb, *argv));
    argv++;
  }
  return mrb_nil_value();
}

static void
check_const_name_sym(mrb_state *mrb, mrb_sym id)
{
  mrb_int len;
  const char *name = mrb_sym_name_len(mrb, id, &len);
  if (!mrb_const_name_p(mrb, name, len)) {
    mrb_name_error(mrb, id, "wrong constant name %n", id);
  }
}

static mrb_value
mrb_mod_const_defined(mrb_state *mrb, mrb_value mod)
{
  mrb_sym id;
  mrb_bool inherit = TRUE;

  mrb_get_args(mrb, "n|b", &id, &inherit);
  check_const_name_sym(mrb, id);
  if (inherit) {
    return mrb_bool_value(mrb_const_defined(mrb, mod, id));
  }
  return mrb_bool_value(mrb_const_defined_at(mrb, mod, id));
}

static mrb_value
mrb_const_get_sym(mrb_state *mrb, mrb_value mod, mrb_sym id)
{
  check_const_name_sym(mrb, id);
  return mrb_const_get(mrb, mod, id);
}

static mrb_value
mrb_mod_const_get(mrb_state *mrb, mrb_value mod)
{
  mrb_value path = mrb_get_arg1(mrb);
  mrb_sym id;
  char *ptr;
  mrb_int off, end, len;

  if (mrb_symbol_p(path)) {
    /* const get with symbol */
    id = mrb_symbol(path);
    return mrb_const_get_sym(mrb, mod, id);
  }

  /* const get with class path string */
  path = mrb_ensure_string_type(mrb, path);
  ptr = RSTRING_PTR(path);
  len = RSTRING_LEN(path);
  off = 0;

  while (off < len) {
    end = mrb_str_index_lit(mrb, path, "::", off);
    end = (end == -1) ? len : end;
    id = mrb_intern(mrb, ptr+off, end-off);
    mod = mrb_const_get_sym(mrb, mod, id);
    if (end == len)
      off = end;
    else {
      off = end + 2;
      if (off == len) {         /* trailing "::" */
        mrb_name_error(mrb, id, "wrong constant name '%v'", path);
      }
    }
  }

  return mod;
}

static mrb_value
mrb_mod_const_set(mrb_state *mrb, mrb_value mod)
{
  mrb_sym id;
  mrb_value value;

  mrb_get_args(mrb, "no", &id, &value);
  check_const_name_sym(mrb, id);
  mrb_const_set(mrb, mod, id, value);
  return value;
}

static mrb_value
mrb_mod_remove_const(mrb_state *mrb, mrb_value mod)
{
  mrb_sym id;
  mrb_value val;

  mrb_get_args(mrb, "n", &id);
  check_const_name_sym(mrb, id);
  val = mrb_iv_remove(mrb, mod, id);
  if (mrb_undef_p(val)) {
    mrb_name_error(mrb, id, "constant %n not defined", id);
  }
  return val;
}

static mrb_value
mrb_mod_const_missing(mrb_state *mrb, mrb_value mod)
{
  mrb_sym sym;

  mrb_get_args(mrb, "n", &sym);

  if (mrb_class_real(mrb_class_ptr(mod)) != mrb->object_class) {
    mrb_name_error(mrb, sym, "uninitialized constant %v::%n", mod, sym);
  }
  else {
    mrb_name_error(mrb, sym, "uninitialized constant %n", sym);
  }
  /* not reached */
  return mrb_nil_value();
}

/* 15.2.2.4.34 */
/*
 *  call-seq:
 *     mod.method_defined?(symbol)    -> true or false
 *
 *  Returns +true+ if the named method is defined by
 *  _mod_ (or its included modules and, if _mod_ is a class,
 *  its ancestors). Public and protected methods are matched.
 *
 *     module A
 *       def method1()  end
 *     end
 *     class B
 *       def method2()  end
 *     end
 *     class C < B
 *       include A
 *       def method3()  end
 *     end
 *
 *     A.method_defined? :method1    #=> true
 *     C.method_defined? "method1"   #=> true
 *     C.method_defined? "method2"   #=> true
 *     C.method_defined? "method3"   #=> true
 *     C.method_defined? "method4"   #=> false
 */

static mrb_value
mrb_mod_method_defined(mrb_state *mrb, mrb_value mod)
{
  mrb_sym id;

  mrb_get_args(mrb, "n", &id);
  return mrb_bool_value(mrb_obj_respond_to(mrb, mrb_class_ptr(mod), id));
}

static mrb_value
mod_define_method(mrb_state *mrb, mrb_value self)
{
  struct RClass *c = mrb_class_ptr(self);
  struct RProc *p;
  mrb_method_t m;
  mrb_sym mid;
  mrb_value proc = mrb_undef_value();
  mrb_value blk;

  mrb_get_args(mrb, "n|o&", &mid, &proc, &blk);
  switch (mrb_type(proc)) {
    case MRB_TT_PROC:
      blk = proc;
      break;
    case MRB_TT_UNDEF:
      /* ignored */
      break;
    default:
      mrb_raisef(mrb, E_TYPE_ERROR, "wrong argument type %T (expected Proc)", proc);
      break;
  }
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  p = (struct RProc*)mrb_obj_alloc(mrb, MRB_TT_PROC, mrb->proc_class);
  mrb_proc_copy(p, mrb_proc_ptr(blk));
  p->flags |= MRB_PROC_STRICT;
  MRB_METHOD_FROM_PROC(m, p);
  mrb_define_method_raw(mrb, c, mid, m);
  return mrb_symbol_value(mid);
}

static mrb_value
top_define_method(mrb_state *mrb, mrb_value self)
{
  return mod_define_method(mrb, mrb_obj_value(mrb->object_class));
}

static mrb_value
mrb_mod_eqq(mrb_state *mrb, mrb_value mod)
{
  mrb_value obj = mrb_get_arg1(mrb);
  mrb_bool eqq;

  eqq = mrb_obj_is_kind_of(mrb, obj, mrb_class_ptr(mod));

  return mrb_bool_value(eqq);
}

static mrb_value
mrb_mod_dup(mrb_state *mrb, mrb_value self)
{
  mrb_value mod = mrb_obj_clone(mrb, self);
  MRB_UNSET_FROZEN_FLAG(mrb_obj_ptr(mod));
  return mod;
}

static mrb_value
mrb_mod_module_function(mrb_state *mrb, mrb_value mod)
{
  mrb_value *argv;
  mrb_int argc, i;
  mrb_sym mid;
  mrb_method_t m;
  struct RClass *rclass;
  int ai;

  mrb_check_type(mrb, mod, MRB_TT_MODULE);

  mrb_get_args(mrb, "*", &argv, &argc);
  if (argc == 0) {
    /* set MODFUNC SCOPE if implemented */
    return mod;
  }

  /* set PRIVATE method visibility if implemented */
  /* mrb_mod_dummy_visibility(mrb, mod); */

  for (i=0; i<argc; i++) {
    mrb_check_type(mrb, argv[i], MRB_TT_SYMBOL);

    mid = mrb_symbol(argv[i]);
    rclass = mrb_class_ptr(mod);
    m = mrb_method_search(mrb, rclass, mid);

    prepare_singleton_class(mrb, (struct RBasic*)rclass);
    ai = mrb_gc_arena_save(mrb);
    mrb_define_method_raw(mrb, rclass->c, mid, m);
    mrb_gc_arena_restore(mrb, ai);
  }

  return mod;
}

/* implementation of __id__ */
mrb_value mrb_obj_id_m(mrb_state *mrb, mrb_value self);
/* implementation of instance_eval */
mrb_value mrb_obj_instance_eval(mrb_state*, mrb_value);

static mrb_value
inspect_main(mrb_state *mrb, mrb_value mod)
{
  return mrb_str_new_lit(mrb, "main");
}

static const mrb_code new_iseq[] = {
  OP_ENTER, 0x0, 0x10, 0x1,  /* OP_ENTER     0:0:1:0:0:0:1 */
  OP_LOADSELF, 0x3,          /* OP_LOADSELF  R3 */
  OP_SEND, 0x3, 0x0, 0x0,    /* OP_SEND      R3  :allocate  0 */
  OP_MOVE, 0x0, 0x3,         /* OP_MOVE      R0  R3 */
  OP_MOVE, 0x4, 0x1,         /* OP_MOVE      R4  R1 */
  OP_MOVE, 0x5, 0x2,         /* OP_MOVE      R5  R2 */
  OP_SENDVB, 0x3, 0x1,       /* OP_SENDVB    R4  :initialize */
  OP_RETURN, 0x0             /* OP_RETURN    R0 */
};

static void
init_class_new(mrb_state *mrb, struct RClass *cls)
{
  struct RProc *p;
  mrb_method_t m;
  mrb_irep *new_irep = (mrb_irep*)mrb_malloc(mrb, sizeof(mrb_irep));
  static const mrb_irep mrb_irep_zero = { 0 };

  *new_irep = mrb_irep_zero;
  new_irep->syms = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym)*2);
  new_irep->syms[0] = mrb_intern_lit(mrb, "allocate");
  new_irep->syms[1] = mrb_intern_lit(mrb, "initialize");
  new_irep->slen = 2;
  new_irep->flags = MRB_ISEQ_NO_FREE;
  new_irep->iseq = new_iseq;
  new_irep->ilen = sizeof(new_iseq);
  new_irep->nregs = 6;
  new_irep->nlocals = 3;
  p = mrb_proc_new(mrb, new_irep);
  MRB_METHOD_FROM_PROC(m, p);
  mrb_define_method_raw(mrb, cls, mrb_intern_lit(mrb, "new"), m);
}

void
mrb_init_class(mrb_state *mrb)
{
  struct RClass *bob;           /* BasicObject */
  struct RClass *obj;           /* Object */
  struct RClass *mod;           /* Module */
  struct RClass *cls;           /* Class */

  /* boot class hierarchy */
  bob = boot_defclass(mrb, 0);
  obj = boot_defclass(mrb, bob); mrb->object_class = obj;
  mod = boot_defclass(mrb, obj); mrb->module_class = mod;/* obj -> mod */
  cls = boot_defclass(mrb, mod); mrb->class_class = cls; /* obj -> cls */
  /* fix-up loose ends */
  bob->c = obj->c = mod->c = cls->c = cls;
  make_metaclass(mrb, bob);
  make_metaclass(mrb, obj);
  make_metaclass(mrb, mod);
  make_metaclass(mrb, cls);

  /* name basic classes */
  mrb_define_const(mrb, bob, "BasicObject", mrb_obj_value(bob));
  mrb_define_const(mrb, obj, "Object",      mrb_obj_value(obj));
  mrb_define_const(mrb, obj, "Module",      mrb_obj_value(mod));
  mrb_define_const(mrb, obj, "Class",       mrb_obj_value(cls));

  /* name each classes */
  mrb_class_name_class(mrb, NULL, bob, mrb_intern_lit(mrb, "BasicObject"));
  mrb_class_name_class(mrb, NULL, obj, mrb_intern_lit(mrb, "Object")); /* 15.2.1 */
  mrb_class_name_class(mrb, NULL, mod, mrb_intern_lit(mrb, "Module")); /* 15.2.2 */
  mrb_class_name_class(mrb, NULL, cls, mrb_intern_lit(mrb, "Class"));  /* 15.2.3 */

  mrb->proc_class = mrb_define_class(mrb, "Proc", mrb->object_class);  /* 15.2.17 */
  MRB_SET_INSTANCE_TT(mrb->proc_class, MRB_TT_PROC);

  MRB_SET_INSTANCE_TT(cls, MRB_TT_CLASS);
  mrb_define_method(mrb, bob, "initialize",              mrb_bob_init,             MRB_ARGS_NONE());
  mrb_define_method(mrb, bob, "!",                       mrb_bob_not,              MRB_ARGS_NONE());
  mrb_define_method(mrb, bob, "==",                      mrb_obj_equal_m,          MRB_ARGS_REQ(1)); /* 15.3.1.3.1  */
  mrb_define_method(mrb, bob, "__id__",                  mrb_obj_id_m,             MRB_ARGS_NONE()); /* 15.3.1.3.4  */
  mrb_define_method(mrb, bob, "__send__",                mrb_f_send,               MRB_ARGS_REQ(1)|MRB_ARGS_REST()|MRB_ARGS_BLOCK());  /* 15.3.1.3.5  */
  mrb_define_method(mrb, bob, "equal?",                  mrb_obj_equal_m,          MRB_ARGS_REQ(1)); /* 15.3.1.3.11 */
  mrb_define_method(mrb, bob, "instance_eval",           mrb_obj_instance_eval,    MRB_ARGS_OPT(1)|MRB_ARGS_BLOCK());  /* 15.3.1.3.18 */

  mrb_define_class_method(mrb, cls, "new",               mrb_class_new_class,      MRB_ARGS_OPT(1)|MRB_ARGS_BLOCK());
  mrb_define_method(mrb, cls, "allocate",                mrb_instance_alloc,       MRB_ARGS_NONE());
  mrb_define_method(mrb, cls, "superclass",              mrb_class_superclass,     MRB_ARGS_NONE()); /* 15.2.3.3.4 */
  mrb_define_method(mrb, cls, "initialize",              mrb_class_initialize,     MRB_ARGS_OPT(1)); /* 15.2.3.3.1 */
  mrb_define_method(mrb, cls, "inherited",               mrb_bob_init,             MRB_ARGS_REQ(1));

  init_class_new(mrb, cls);

  MRB_SET_INSTANCE_TT(mod, MRB_TT_MODULE);
  mrb_define_method(mrb, mod, "extend_object",           mrb_mod_extend_object,    MRB_ARGS_REQ(1)); /* 15.2.2.4.25 */
  mrb_define_method(mrb, mod, "extended",                mrb_bob_init,             MRB_ARGS_REQ(1)); /* 15.2.2.4.26 */
  mrb_define_method(mrb, mod, "prepended",               mrb_bob_init,             MRB_ARGS_REQ(1));
  mrb_define_method(mrb, mod, "prepend_features",        mrb_mod_prepend_features, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, mod, "include?",                mrb_mod_include_p,        MRB_ARGS_REQ(1)); /* 15.2.2.4.28 */
  mrb_define_method(mrb, mod, "append_features",         mrb_mod_append_features,  MRB_ARGS_REQ(1)); /* 15.2.2.4.10 */
  mrb_define_method(mrb, mod, "class_eval",              mrb_mod_module_eval,      MRB_ARGS_ANY());  /* 15.2.2.4.15 */
  mrb_define_method(mrb, mod, "included",                mrb_bob_init,             MRB_ARGS_REQ(1)); /* 15.2.2.4.29 */
  mrb_define_method(mrb, mod, "initialize",              mrb_mod_initialize,       MRB_ARGS_NONE()); /* 15.2.2.4.31 */
  mrb_define_method(mrb, mod, "module_eval",             mrb_mod_module_eval,      MRB_ARGS_ANY());  /* 15.2.2.4.35 */
  mrb_define_method(mrb, mod, "module_function",         mrb_mod_module_function,  MRB_ARGS_ANY());
  mrb_define_method(mrb, mod, "private",                 mrb_mod_dummy_visibility, MRB_ARGS_ANY());  /* 15.2.2.4.36 */
  mrb_define_method(mrb, mod, "protected",               mrb_mod_dummy_visibility, MRB_ARGS_ANY());  /* 15.2.2.4.37 */
  mrb_define_method(mrb, mod, "public",                  mrb_mod_dummy_visibility, MRB_ARGS_ANY());  /* 15.2.2.4.38 */
  mrb_define_method(mrb, mod, "attr_reader",             mrb_mod_attr_reader,      MRB_ARGS_ANY());  /* 15.2.2.4.13 */
  mrb_define_method(mrb, mod, "attr_writer",             mrb_mod_attr_writer,      MRB_ARGS_ANY());  /* 15.2.2.4.14 */
  mrb_define_method(mrb, mod, "to_s",                    mrb_mod_to_s,             MRB_ARGS_NONE());
  mrb_define_method(mrb, mod, "inspect",                 mrb_mod_to_s,             MRB_ARGS_NONE());
  mrb_define_method(mrb, mod, "alias_method",            mrb_mod_alias,            MRB_ARGS_ANY());  /* 15.2.2.4.8 */
  mrb_define_method(mrb, mod, "ancestors",               mrb_mod_ancestors,        MRB_ARGS_NONE()); /* 15.2.2.4.9 */
  mrb_define_method(mrb, mod, "undef_method",            mrb_mod_undef,            MRB_ARGS_ANY());  /* 15.2.2.4.41 */
  mrb_define_method(mrb, mod, "const_defined?",          mrb_mod_const_defined,    MRB_ARGS_ARG(1,1)); /* 15.2.2.4.20 */
  mrb_define_method(mrb, mod, "const_get",               mrb_mod_const_get,        MRB_ARGS_REQ(1)); /* 15.2.2.4.21 */
  mrb_define_method(mrb, mod, "const_set",               mrb_mod_const_set,        MRB_ARGS_REQ(2)); /* 15.2.2.4.23 */
  mrb_define_method(mrb, mod, "remove_const",            mrb_mod_remove_const,     MRB_ARGS_REQ(1)); /* 15.2.2.4.40 */
  mrb_define_method(mrb, mod, "const_missing",           mrb_mod_const_missing,    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, mod, "method_defined?",         mrb_mod_method_defined,   MRB_ARGS_REQ(1)); /* 15.2.2.4.34 */
  mrb_define_method(mrb, mod, "define_method",           mod_define_method,        MRB_ARGS_ARG(1,1));
  mrb_define_method(mrb, mod, "===",                     mrb_mod_eqq,              MRB_ARGS_REQ(1)); /* 15.2.2.4.7 */
  mrb_define_method(mrb, mod, "dup",                     mrb_mod_dup,              MRB_ARGS_NONE());

  mrb_undef_method(mrb, cls, "append_features");
  mrb_undef_method(mrb, cls, "prepend_features");
  mrb_undef_method(mrb, cls, "extend_object");
  mrb_undef_method(mrb, cls, "module_function");

  mrb->top_self = (struct RObject*)mrb_obj_alloc(mrb, MRB_TT_OBJECT, mrb->object_class);
  mrb_define_singleton_method(mrb, mrb->top_self, "inspect", inspect_main, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, mrb->top_self, "to_s", inspect_main, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, mrb->top_self, "define_method", top_define_method, MRB_ARGS_ARG(1,1));
}
#include <mruby.h>
#include <mruby/irep.h>
#include <mruby/debug.h>
#include <mruby/opcode.h>
#include <mruby/string.h>
#include <mruby/proc.h>

#ifndef MRB_DISABLE_STDIO
static void
print_r(mrb_state *mrb, mrb_irep *irep, size_t n)
{
  size_t i;

  if (n == 0) return;

  for (i=0; i+1<irep->nlocals; i++) {
    if (irep->lv[i].r == n) {
      mrb_sym sym = irep->lv[i].name;
      printf(" R%d:%s", (int)n, mrb_sym_dump(mrb, sym));
      break;
    }
  }
}

static void
print_lv_a(mrb_state *mrb, mrb_irep *irep, uint16_t a)
{
  if (!irep->lv || a >= irep->nlocals || a == 0) {
    printf("\n");
    return;
  }
  printf("\t;");
  print_r(mrb, irep, a);
  printf("\n");
}

static void
print_lv_ab(mrb_state *mrb, mrb_irep *irep, uint16_t a, uint16_t b)
{
  if (!irep->lv || (a >= irep->nlocals && b >= irep->nlocals) || a+b == 0) {
    printf("\n");
    return;
  }
  printf("\t;");
  if (a > 0) print_r(mrb, irep, a);
  if (b > 0) print_r(mrb, irep, b);
  printf("\n");
}

static void
print_header(mrb_state *mrb, mrb_irep *irep, ptrdiff_t i)
{
  int32_t line;

  line = mrb_debug_get_line(mrb, irep, i);
  if (line < 0) {
    printf("      ");
  }
  else {
    printf("%5d ", line);
  }

  printf("%03d ", (int)i);
}

#define CASE_CODEDUMP(insn,ops) case insn: FETCH_ ## ops (); L_ ## insn

static void
codedump(mrb_state *mrb, mrb_irep *irep)
{
  int ai;
  const mrb_code *pc, *pcend;
  mrb_code ins;
  const char *file = NULL, *next_file;

  if (!irep) return;
  printf("irep %p nregs=%d nlocals=%d pools=%d syms=%d reps=%d iseq=%d\n", (void*)irep,
         irep->nregs, irep->nlocals, (int)irep->plen, (int)irep->slen, (int)irep->rlen, (int)irep->ilen);

  if (irep->lv) {
    int i;

    printf("local variable names:\n");
    for (i = 1; i < irep->nlocals; ++i) {
      char const *s = mrb_sym_dump(mrb, irep->lv[i - 1].name);
      int n = irep->lv[i - 1].r ? irep->lv[i - 1].r : i;
      printf("  R%d:%s\n", n, s ? s : "");
    }
  }

  pc = irep->iseq;
  pcend = pc + irep->ilen;
  while (pc < pcend) {
    ptrdiff_t i;
    uint32_t a;
    uint16_t b;
    uint8_t c;

    ai = mrb_gc_arena_save(mrb);

    i = pc - irep->iseq;
    next_file = mrb_debug_get_filename(mrb, irep, i);
    if (next_file && file != next_file) {
      printf("file: %s\n", next_file);
      file = next_file;
    }
    print_header(mrb, irep, i);
    ins = READ_B();
    switch (ins) {
    CASE_CODEDUMP(OP_NOP, Z):
      printf("OP_NOP\n");
      break;
    CASE_CODEDUMP(OP_MOVE, BB):
      printf("OP_MOVE\tR%d\tR%d\t", a, b);
      print_lv_ab(mrb, irep, a, b);
      break;
    CASE_CODEDUMP(OP_LOADL, BB):
      {
        mrb_value v = irep->pool[b];
        mrb_value s = mrb_inspect(mrb, v);
        printf("OP_LOADL\tR%d\tL(%d)\t; %s", a, b, RSTRING_PTR(s));
      }
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADI, BB):
      printf("OP_LOADI\tR%d\t%d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADINEG, BB):
      printf("OP_LOADI\tR%d\t-%d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADI16, BS):
      printf("OP_LOADI16\tR%d\t%d\t", a, (int)(int16_t)b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADI__1, B):
      printf("OP_LOADI__1\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADI_0, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_1, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_2, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_3, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_4, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_5, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_6, B): goto L_LOADI;
    CASE_CODEDUMP(OP_LOADI_7, B):
    L_LOADI:
      printf("OP_LOADI_%d\tR%d\t\t", ins-(int)OP_LOADI_0, a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADSYM, BB):
      printf("OP_LOADSYM\tR%d\t:%s\t", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADNIL, B):
      printf("OP_LOADNIL\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADSELF, B):
      printf("OP_LOADSELF\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADT, B):
      printf("OP_LOADT\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LOADF, B):
      printf("OP_LOADF\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETGV, BB):
      printf("OP_GETGV\tR%d\t:%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETGV, BB):
      printf("OP_SETGV\t:%s\tR%d", mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETSV, BB):
      printf("OP_GETSV\tR%d\t:%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETSV, BB):
      printf("OP_SETSV\t:%s\tR%d", mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETCONST, BB):
      printf("OP_GETCONST\tR%d\t:%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETCONST, BB):
      printf("OP_SETCONST\t:%s\tR%d", mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETMCNST, BB):
      printf("OP_GETMCNST\tR%d\tR%d::%s", a, a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETMCNST, BB):
      printf("OP_SETMCNST\tR%d::%s\tR%d", a+1, mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETIV, BB):
      printf("OP_GETIV\tR%d\t%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETIV, BB):
      printf("OP_SETIV\t%s\tR%d", mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETUPVAR, BBB):
      printf("OP_GETUPVAR\tR%d\t%d\t%d", a, b, c);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETUPVAR, BBB):
      printf("OP_SETUPVAR\tR%d\t%d\t%d", a, b, c);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_GETCV, BB):
      printf("OP_GETCV\tR%d\t%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SETCV, BB):
      printf("OP_SETCV\t%s\tR%d", mrb_sym_dump(mrb, irep->syms[b]), a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_JMP, S):
      printf("OP_JMP\t\t%03d\n", a);
      break;
    CASE_CODEDUMP(OP_JMPIF, BS):
      printf("OP_JMPIF\tR%d\t%03d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_JMPNOT, BS):
      printf("OP_JMPNOT\tR%d\t%03d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_JMPNIL, BS):
      printf("OP_JMPNIL\tR%d\t%03d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SENDV, BB):
      printf("OP_SENDV\tR%d\t:%s\n", a, mrb_sym_dump(mrb, irep->syms[b]));
      break;
    CASE_CODEDUMP(OP_SENDVB, BB):
      printf("OP_SENDVB\tR%d\t:%s\n", a, mrb_sym_dump(mrb, irep->syms[b]));
      break;
    CASE_CODEDUMP(OP_SEND, BBB):
      printf("OP_SEND\tR%d\t:%s\t%d\n", a, mrb_sym_dump(mrb, irep->syms[b]), c);
      break;
    CASE_CODEDUMP(OP_SENDB, BBB):
      printf("OP_SENDB\tR%d\t:%s\t%d\n", a, mrb_sym_dump(mrb, irep->syms[b]), c);
      break;
    CASE_CODEDUMP(OP_CALL, Z):
      printf("OP_CALL\n");
      break;
    CASE_CODEDUMP(OP_SUPER, BB):
      printf("OP_SUPER\tR%d\t%d\n", a, b);
      break;
    CASE_CODEDUMP(OP_ARGARY, BS):
      printf("OP_ARGARY\tR%d\t%d:%d:%d:%d (%d)", a,
             (b>>11)&0x3f,
             (b>>10)&0x1,
             (b>>5)&0x1f,
             (b>>4)&0x1,
             (b>>0)&0xf);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_ENTER, W):
      printf("OP_ENTER\t%d:%d:%d:%d:%d:%d:%d\n",
             MRB_ASPEC_REQ(a),
             MRB_ASPEC_OPT(a),
             MRB_ASPEC_REST(a),
             MRB_ASPEC_POST(a),
             MRB_ASPEC_KEY(a),
             MRB_ASPEC_KDICT(a),
             MRB_ASPEC_BLOCK(a));
      break;
    CASE_CODEDUMP(OP_KEY_P, BB):
      printf("OP_KEY_P\tR%d\t:%s\t", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_KEYEND, Z):
      printf("OP_KEYEND\n");
      break;
    CASE_CODEDUMP(OP_KARG, BB):
      printf("OP_KARG\tR%d\t:%s\t", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_RETURN, B):
      printf("OP_RETURN\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_RETURN_BLK, B):
      printf("OP_RETURN_BLK\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_BREAK, B):
      printf("OP_BREAK\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_BLKPUSH, BS):
      printf("OP_BLKPUSH\tR%d\t%d:%d:%d:%d (%d)", a,
             (b>>11)&0x3f,
             (b>>10)&0x1,
             (b>>5)&0x1f,
             (b>>4)&0x1,
             (b>>0)&0xf);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_LAMBDA, BB):
      printf("OP_LAMBDA\tR%d\tI(%d:%p)\n", a, b, irep->reps[b]);
      break;
    CASE_CODEDUMP(OP_BLOCK, BB):
      printf("OP_BLOCK\tR%d\tI(%d:%p)\n", a, b, irep->reps[b]);
      break;
    CASE_CODEDUMP(OP_METHOD, BB):
      printf("OP_METHOD\tR%d\tI(%d:%p)\n", a, b, irep->reps[b]);
      break;
    CASE_CODEDUMP(OP_RANGE_INC, B):
      printf("OP_RANGE_INC\tR%d\n", a);
      break;
    CASE_CODEDUMP(OP_RANGE_EXC, B):
      printf("OP_RANGE_EXC\tR%d\n", a);
      break;
    CASE_CODEDUMP(OP_DEF, BB):
      printf("OP_DEF\tR%d\t:%s\n", a, mrb_sym_dump(mrb, irep->syms[b]));
      break;
    CASE_CODEDUMP(OP_UNDEF, B):
      printf("OP_UNDEF\t:%s\n", mrb_sym_dump(mrb, irep->syms[a]));
      break;
    CASE_CODEDUMP(OP_ALIAS, BB):
      printf("OP_ALIAS\t:%s\t%s\n", mrb_sym_dump(mrb, irep->syms[a]), mrb_sym_dump(mrb, irep->syms[b]));
      break;
    CASE_CODEDUMP(OP_ADD, B):
      printf("OP_ADD\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_ADDI, BB):
      printf("OP_ADDI\tR%d\t%d\n", a, b);
      break;
    CASE_CODEDUMP(OP_SUB, B):
      printf("OP_SUB\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_SUBI, BB):
      printf("OP_SUBI\tR%d\t%d\n", a, b);
      break;
    CASE_CODEDUMP(OP_MUL, B):
      printf("OP_MUL\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_DIV, B):
      printf("OP_DIV\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_LT, B):
      printf("OP_LT\t\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_LE, B):
      printf("OP_LE\t\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_GT, B):
      printf("OP_GT\t\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_GE, B):
      printf("OP_GE\t\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_EQ, B):
      printf("OP_EQ\t\tR%d\t\n", a);
      break;
    CASE_CODEDUMP(OP_ARRAY, BB):
      printf("OP_ARRAY\tR%d\t%d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_ARRAY2, BBB):
      printf("OP_ARRAY\tR%d\tR%d\t%d\t", a, b, c);
      print_lv_ab(mrb, irep, a, b);
      break;
    CASE_CODEDUMP(OP_ARYCAT, B):
      printf("OP_ARYCAT\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_ARYPUSH, B):
      printf("OP_ARYPUSH\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_ARYDUP, B):
      printf("OP_ARYDUP\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_AREF, BBB):
      printf("OP_AREF\tR%d\tR%d\t%d", a, b, c);
      print_lv_ab(mrb, irep, a, b);
      break;
    CASE_CODEDUMP(OP_ASET, BBB):
      printf("OP_ASET\tR%d\tR%d\t%d", a, b, c);
      print_lv_ab(mrb, irep, a, b);
      break;
    CASE_CODEDUMP(OP_APOST, BBB):
      printf("OP_APOST\tR%d\t%d\t%d", a, b, c);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_INTERN, B):
      printf("OP_INTERN\tR%d", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_STRING, BB):
      {
        mrb_value v = irep->pool[b];
        mrb_value s = mrb_str_dump(mrb, mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
        printf("OP_STRING\tR%d\tL(%d)\t; %s", a, b, RSTRING_PTR(s));
      }
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_STRCAT, B):
      printf("OP_STRCAT\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_HASH, BB):
      printf("OP_HASH\tR%d\t%d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_HASHADD, BB):
      printf("OP_HASHADD\tR%d\t%d\t", a, b);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_HASHCAT, B):
      printf("OP_HASHCAT\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;

    CASE_CODEDUMP(OP_OCLASS, B):
      printf("OP_OCLASS\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_CLASS, BB):
      printf("OP_CLASS\tR%d\t:%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_MODULE, BB):
      printf("OP_MODULE\tR%d\t:%s", a, mrb_sym_dump(mrb, irep->syms[b]));
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_EXEC, BB):
      printf("OP_EXEC\tR%d\tI(%d:%p)", a, b, irep->reps[b]);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_SCLASS, B):
      printf("OP_SCLASS\tR%d\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_TCLASS, B):
      printf("OP_TCLASS\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_ERR, B):
      {
        mrb_value v = irep->pool[a];
        mrb_value s = mrb_str_dump(mrb, mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
        printf("OP_ERR\t%s\n", RSTRING_PTR(s));
      }
      break;
    CASE_CODEDUMP(OP_EPUSH, B):
      printf("OP_EPUSH\t\t:I(%d:%p)\n", a, irep->reps[a]);
      break;
    CASE_CODEDUMP(OP_ONERR, S):
      printf("OP_ONERR\t%03d\n", a);
      break;
    CASE_CODEDUMP(OP_EXCEPT, B):
      printf("OP_EXCEPT\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_RESCUE, BB):
      printf("OP_RESCUE\tR%d\tR%d", a, b);
      print_lv_ab(mrb, irep, a, b);
      break;
    CASE_CODEDUMP(OP_RAISE, B):
      printf("OP_RAISE\tR%d\t\t", a);
      print_lv_a(mrb, irep, a);
      break;
    CASE_CODEDUMP(OP_POPERR, B):
      printf("OP_POPERR\t%d\t\t\n", a);
      break;
    CASE_CODEDUMP(OP_EPOP, B):
      printf("OP_EPOP\t%d\n", a);
      break;

    CASE_CODEDUMP(OP_DEBUG, BBB):
      printf("OP_DEBUG\t%d\t%d\t%d\n", a, b, c);
      break;

    CASE_CODEDUMP(OP_STOP, Z):
      printf("OP_STOP\n");
      break;

    CASE_CODEDUMP(OP_EXT1, Z):
      ins = READ_B();
      printf("OP_EXT1\n");
      print_header(mrb, irep, pc-irep->iseq-2);
      switch (ins) {
#define OPCODE(i,x) case OP_ ## i: FETCH_ ## x ## _1 (); goto L_OP_ ## i;
#include "mruby/ops.h"
#undef OPCODE
      }
      break;
    CASE_CODEDUMP(OP_EXT2, Z):
      ins = READ_B();
      printf("OP_EXT2\n");
      print_header(mrb, irep, pc-irep->iseq-2);
      switch (ins) {
#define OPCODE(i,x) case OP_ ## i: FETCH_ ## x ## _2 (); goto L_OP_ ## i;
#include "mruby/ops.h"
#undef OPCODE
      }
      break;
    CASE_CODEDUMP(OP_EXT3, Z):
      ins = READ_B();
      printf("OP_EXT3\n");
      print_header(mrb, irep, pc-irep->iseq-2);
      switch (ins) {
#define OPCODE(i,x) case OP_ ## i: FETCH_ ## x ## _3 (); goto L_OP_ ## i;
#include "mruby/ops.h"
#undef OPCODE
      }
      break;

    default:
      printf("OP_unknown (0x%x)\n", ins);
      break;
    }
    mrb_gc_arena_restore(mrb, ai);
  }
  printf("\n");
}

static void
codedump_recur(mrb_state *mrb, mrb_irep *irep)
{
  int i;

  codedump(mrb, irep);
  for (i=0; i<irep->rlen; i++) {
    codedump_recur(mrb, irep->reps[i]);
  }
}
#endif

void
mrb_codedump_all(mrb_state *mrb, struct RProc *proc)
{
#ifndef MRB_DISABLE_STDIO
  codedump_recur(mrb, proc->body.irep);
#endif
}
/*
** compar.c - Comparable module
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>

void
mrb_init_comparable(mrb_state *mrb)
{
  mrb_define_module(mrb, "Comparable");  /* 15.3.3 */
}
/*
** crc.c - calculate CRC
**
** See Copyright Notice in mruby.h
*/

#include <limits.h>
#include <stdint.h>
#include <stddef.h>

/* Calculate CRC (CRC-16-CCITT)
**
**  0000_0000_0000_0000_0000_0000_0000_0000
**          ^|------- CRC -------|- work --|
**        carry
*/
#define  CRC_16_CCITT       0x11021ul        /* x^16+x^12+x^5+1 */
#define  CRC_XOR_PATTERN    (CRC_16_CCITT << 8)
#define  CRC_CARRY_BIT      (0x01000000)

uint16_t
calc_crc_16_ccitt(const uint8_t *src, size_t nbytes, uint16_t crc)
{
  size_t ibyte;
  uint32_t ibit;
  uint32_t crcwk = crc << 8;

  for (ibyte = 0; ibyte < nbytes; ibyte++) {
    crcwk |= *src++;
    for (ibit = 0; ibit < CHAR_BIT; ibit++) {
      crcwk <<= 1;
      if (crcwk & CRC_CARRY_BIT) {
        crcwk ^= CRC_XOR_PATTERN;
      }
    }
  }
  return (uint16_t)(crcwk >> 8);
}

#include <string.h>
#include <mruby.h>
#include <mruby/irep.h>
#include <mruby/debug.h>

static mrb_irep_debug_info_file*
get_file(mrb_irep_debug_info *info, uint32_t pc)
{
  mrb_irep_debug_info_file **ret;
  int32_t count;

  if (pc >= info->pc_count) { return NULL; }
  /* get upper bound */
  ret = info->files;
  count =  info->flen;
  while (count > 0) {
    int32_t step = count / 2;
    mrb_irep_debug_info_file **it = ret + step;
    if (!(pc < (*it)->start_pos)) {
      ret = it + 1;
      count -= step + 1;
    }
    else { count = step; }
  }

  --ret;

  /* check returning file exists inside debug info */
  mrb_assert(info->files <= ret && ret < (info->files + info->flen));
  /* check pc is within the range of returning file */
  mrb_assert((*ret)->start_pos <= pc &&
             pc < (((ret + 1 - info->files) < info->flen)
                   ? (*(ret+1))->start_pos : info->pc_count));

  return *ret;
}

static mrb_debug_line_type
select_line_type(const uint16_t *lines, size_t lines_len)
{
  size_t line_count = 0;
  int prev_line = -1;
  size_t i;
  for (i = 0; i < lines_len; ++i) {
    if (lines[i] != prev_line) {
      ++line_count;
    }
  }
  return (sizeof(uint16_t) * lines_len) <= (sizeof(mrb_irep_debug_info_line) * line_count)
      ? mrb_debug_line_ary : mrb_debug_line_flat_map;
}

MRB_API char const*
mrb_debug_get_filename(mrb_state *mrb, mrb_irep *irep, ptrdiff_t pc)
{
  if (irep && pc >= 0 && pc < irep->ilen) {
    mrb_irep_debug_info_file* f = NULL;
    if (!irep->debug_info) return NULL;
    else if ((f = get_file(irep->debug_info, (uint32_t)pc))) {
      return mrb_sym_name_len(mrb, f->filename_sym, NULL);
    }
  }
  return NULL;
}

MRB_API int32_t
mrb_debug_get_line(mrb_state *mrb, mrb_irep *irep, ptrdiff_t pc)
{
  if (irep && pc >= 0 && pc < irep->ilen) {
    mrb_irep_debug_info_file* f = NULL;
    if (!irep->debug_info) {
      return -1;
    }
    else if ((f = get_file(irep->debug_info, (uint32_t)pc))) {
      switch (f->line_type) {
        case mrb_debug_line_ary:
          mrb_assert(f->start_pos <= pc && pc < (f->start_pos + f->line_entry_count));
          return f->lines.ary[pc - f->start_pos];

        case mrb_debug_line_flat_map: {
          /* get upper bound */
          mrb_irep_debug_info_line *ret = f->lines.flat_map;
          uint32_t count = f->line_entry_count;
          while (count > 0) {
            int32_t step = count / 2;
            mrb_irep_debug_info_line *it = ret + step;
            if (!(pc < it->start_pos)) {
              ret = it + 1;
              count -= step + 1;
            }
            else { count = step; }
          }

          --ret;

          /* check line entry pointer range */
          mrb_assert(f->lines.flat_map <= ret && ret < (f->lines.flat_map + f->line_entry_count));
          /* check pc range */
          mrb_assert(ret->start_pos <= pc &&
                     pc < (((uint32_t)(ret + 1 - f->lines.flat_map) < f->line_entry_count)
                           ? (ret+1)->start_pos : irep->debug_info->pc_count));

          return ret->line;
        }
      }
    }
  }
  return -1;
}

MRB_API mrb_irep_debug_info*
mrb_debug_info_alloc(mrb_state *mrb, mrb_irep *irep)
{
  static const mrb_irep_debug_info initial = { 0, 0, NULL };
  mrb_irep_debug_info *ret;

  mrb_assert(!irep->debug_info);
  ret = (mrb_irep_debug_info *)mrb_malloc(mrb, sizeof(*ret));
  *ret = initial;
  irep->debug_info = ret;
  return ret;
}

MRB_API mrb_irep_debug_info_file*
mrb_debug_info_append_file(mrb_state *mrb, mrb_irep_debug_info *d,
                           const char *filename, uint16_t *lines,
                           uint32_t start_pos, uint32_t end_pos)
{
  mrb_irep_debug_info_file *f;
  uint32_t file_pc_count;
  size_t fn_len;
  uint32_t i;

  if (!d) return NULL;
  if (start_pos == end_pos) return NULL;

  mrb_assert(filename);
  mrb_assert(lines);

  if (d->flen > 0) {
    const char *fn = mrb_sym_name_len(mrb, d->files[d->flen - 1]->filename_sym, NULL);
    if (strcmp(filename, fn) == 0)
      return NULL;
  }

  f = (mrb_irep_debug_info_file*)mrb_malloc(mrb, sizeof(*f));
  d->files = (mrb_irep_debug_info_file**)(
          d->files
          ? mrb_realloc(mrb, d->files, sizeof(mrb_irep_debug_info_file*) * (d->flen + 1))
          : mrb_malloc(mrb, sizeof(mrb_irep_debug_info_file*)));
  d->files[d->flen++] = f;

  file_pc_count = end_pos - start_pos;

  f->start_pos = start_pos;
  d->pc_count = end_pos;

  fn_len = strlen(filename);
  f->filename_sym = mrb_intern(mrb, filename, fn_len);

  f->line_type = select_line_type(lines + start_pos, end_pos - start_pos);
  f->lines.ptr = NULL;

  switch (f->line_type) {
    case mrb_debug_line_ary:
      f->line_entry_count = file_pc_count;
      f->lines.ary = (uint16_t*)mrb_malloc(mrb, sizeof(uint16_t) * file_pc_count);
      for (i = 0; i < file_pc_count; ++i) {
        f->lines.ary[i] = lines[start_pos + i];
      }
      break;

    case mrb_debug_line_flat_map: {
      uint16_t prev_line = 0;
      mrb_irep_debug_info_line m;
      f->lines.flat_map = (mrb_irep_debug_info_line*)mrb_malloc(mrb, sizeof(mrb_irep_debug_info_line) * 1);
      f->line_entry_count = 0;
      for (i = 0; i < file_pc_count; ++i) {
        if (lines[start_pos + i] == prev_line) { continue; }

        f->lines.flat_map = (mrb_irep_debug_info_line*)mrb_realloc(
            mrb, f->lines.flat_map,
            sizeof(mrb_irep_debug_info_line) * (f->line_entry_count + 1));
        m.start_pos = start_pos + i;
        m.line = lines[start_pos + i];
        f->lines.flat_map[f->line_entry_count] = m;

        /* update */
        ++f->line_entry_count;
        prev_line = lines[start_pos + i];
      }
    } break;

    default: mrb_assert(0); break;
  }

  return f;
}

MRB_API void
mrb_debug_info_free(mrb_state *mrb, mrb_irep_debug_info *d)
{
  uint32_t i;

  if (!d) { return; }

  if (d->files) {
    for (i = 0; i < d->flen; ++i) {
      if (d->files[i]) {
        mrb_free(mrb, d->files[i]->lines.ptr);
        mrb_free(mrb, d->files[i]);
      }
    }
    mrb_free(mrb, d->files);
  }
  mrb_free(mrb, d);
}
/*
** dump.c - mruby binary dumper (mrbc binary format)
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <limits.h>
#include <math.h>
#include <mruby/dump.h>
#include <mruby/string.h>
#include <mruby/irep.h>
#include <mruby/numeric.h>
#include <mruby/debug.h>

#ifndef MRB_WITHOUT_FLOAT
#ifdef MRB_USE_FLOAT
#define MRB_FLOAT_FMT "%.9g"
#else
#define MRB_FLOAT_FMT "%.17g"
#endif
#endif

static size_t get_irep_record_size_1(mrb_state *mrb, mrb_irep *irep);

#if UINT32_MAX > SIZE_MAX
# error This code cannot be built on your environment.
#endif

static size_t
write_padding(uint8_t *buf)
{
  const size_t align = MRB_DUMP_ALIGNMENT;
  size_t pad_len = -(intptr_t)buf & (align-1);
  if (pad_len > 0) {
    memset(buf, 0, pad_len);
  }
  return pad_len;
}

static size_t
get_irep_header_size(mrb_state *mrb)
{
  size_t size = 0;

  size += sizeof(uint32_t) * 1;
  size += sizeof(uint16_t) * 3;

  return size;
}

static ptrdiff_t
write_irep_header(mrb_state *mrb, mrb_irep *irep, uint8_t *buf)
{
  uint8_t *cur = buf;

  cur += uint32_to_bin((uint32_t)get_irep_record_size_1(mrb, irep), cur);  /* record size */
  cur += uint16_to_bin((uint16_t)irep->nlocals, cur);  /* number of local variable */
  cur += uint16_to_bin((uint16_t)irep->nregs, cur);  /* number of register variable */
  cur += uint16_to_bin((uint16_t)irep->rlen, cur);  /* number of child irep */

  return cur - buf;
}


static size_t
get_iseq_block_size(mrb_state *mrb, mrb_irep *irep)
{
  size_t size = 0;

  size += sizeof(uint32_t); /* ilen */
  size += sizeof(uint32_t); /* max padding */
  size += sizeof(uint32_t) * irep->ilen; /* iseq(n) */

  return size;
}

static ptrdiff_t
write_iseq_block(mrb_state *mrb, mrb_irep *irep, uint8_t *buf, uint8_t flags)
{
  uint8_t *cur = buf;

  cur += uint32_to_bin(irep->ilen, cur); /* number of opcode */
  cur += write_padding(cur);
  memcpy(cur, irep->iseq, irep->ilen * sizeof(mrb_code));
  cur += irep->ilen * sizeof(mrb_code);

  return cur - buf;
}

#ifndef MRB_WITHOUT_FLOAT
static mrb_value
float_to_str(mrb_state *mrb, mrb_value flt)
{
  mrb_float f = mrb_float(flt);

  if (isinf(f)) {
    return f < 0 ? mrb_str_new_lit(mrb, "I") : mrb_str_new_lit(mrb, "i");
  }
  return  mrb_float_to_str(mrb, flt, MRB_FLOAT_FMT);
}
#endif

static size_t
get_pool_block_size(mrb_state *mrb, mrb_irep *irep)
{
  int pool_no;
  size_t size = 0;
  mrb_value str;

  size += sizeof(uint32_t); /* plen */
  size += irep->plen * (sizeof(uint8_t) + sizeof(uint16_t)); /* len(n) */

  for (pool_no = 0; pool_no < irep->plen; pool_no++) {
    int ai = mrb_gc_arena_save(mrb);

    switch (mrb_type(irep->pool[pool_no])) {
    case MRB_TT_FIXNUM:
      str = mrb_fixnum_to_str(mrb, irep->pool[pool_no], 10);
      {
        mrb_int len = RSTRING_LEN(str);
        mrb_assert_int_fit(mrb_int, len, size_t, SIZE_MAX);
        size += (size_t)len;
      }
      break;

#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      str = float_to_str(mrb, irep->pool[pool_no]);
      {
        mrb_int len = RSTRING_LEN(str);
        mrb_assert_int_fit(mrb_int, len, size_t, SIZE_MAX);
        size += (size_t)len;
      }
      break;
#endif

    case MRB_TT_STRING:
      {
        mrb_int len = RSTRING_LEN(irep->pool[pool_no]);
        mrb_assert_int_fit(mrb_int, len, size_t, SIZE_MAX);
        size += (size_t)len;
      }
      break;

    default:
      break;
    }
    mrb_gc_arena_restore(mrb, ai);
  }

  return size;
}

static ptrdiff_t
write_pool_block(mrb_state *mrb, mrb_irep *irep, uint8_t *buf)
{
  int pool_no;
  uint8_t *cur = buf;
  uint16_t len;
  mrb_value str;
  const char *char_ptr;

  cur += uint32_to_bin(irep->plen, cur); /* number of pool */

  for (pool_no = 0; pool_no < irep->plen; pool_no++) {
    int ai = mrb_gc_arena_save(mrb);

    switch (mrb_type(irep->pool[pool_no])) {
    case MRB_TT_FIXNUM:
      cur += uint8_to_bin(IREP_TT_FIXNUM, cur); /* data type */
      str = mrb_fixnum_to_str(mrb, irep->pool[pool_no], 10);
      break;

#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      cur += uint8_to_bin(IREP_TT_FLOAT, cur); /* data type */
      str = float_to_str(mrb, irep->pool[pool_no]);
      break;
#endif

    case MRB_TT_STRING:
      cur += uint8_to_bin(IREP_TT_STRING, cur); /* data type */
      str = irep->pool[pool_no];
      break;

    default:
      continue;
    }

    char_ptr = RSTRING_PTR(str);
    {
      mrb_int tlen = RSTRING_LEN(str);
      mrb_assert_int_fit(mrb_int, tlen, uint16_t, UINT16_MAX);
      len = (uint16_t)tlen;
    }

    cur += uint16_to_bin(len, cur); /* data length */
    memcpy(cur, char_ptr, (size_t)len);
    cur += len;

    mrb_gc_arena_restore(mrb, ai);
  }

  return cur - buf;
}


static size_t
get_syms_block_size(mrb_state *mrb, mrb_irep *irep)
{
  size_t size = 0;
  int sym_no;
  mrb_int len;

  size += sizeof(uint32_t); /* slen */
  for (sym_no = 0; sym_no < irep->slen; sym_no++) {
    size += sizeof(uint16_t); /* snl(n) */
    if (irep->syms[sym_no] != 0) {
      mrb_sym_name_len(mrb, irep->syms[sym_no], &len);
      size += len + 1; /* sn(n) + null char */
    }
  }

  return size;
}

static ptrdiff_t
write_syms_block(mrb_state *mrb, mrb_irep *irep, uint8_t *buf)
{
  int sym_no;
  uint8_t *cur = buf;
  const char *name;

  cur += uint32_to_bin(irep->slen, cur); /* number of symbol */

  for (sym_no = 0; sym_no < irep->slen; sym_no++) {
    if (irep->syms[sym_no] != 0) {
      mrb_int len;

      name = mrb_sym_name_len(mrb, irep->syms[sym_no], &len);

      mrb_assert_int_fit(mrb_int, len, uint16_t, UINT16_MAX);
      cur += uint16_to_bin((uint16_t)len, cur); /* length of symbol name */
      memcpy(cur, name, len); /* symbol name */
      cur += (uint16_t)len;
      *cur++ = '\0';
    }
    else {
      cur += uint16_to_bin(MRB_DUMP_NULL_SYM_LEN, cur); /* length of symbol name */
    }
  }

  return cur - buf;
}

static size_t
get_irep_record_size_1(mrb_state *mrb, mrb_irep *irep)
{
  size_t size = 0;

  size += get_irep_header_size(mrb);
  size += get_iseq_block_size(mrb, irep);
  size += get_pool_block_size(mrb, irep);
  size += get_syms_block_size(mrb, irep);
  return size;
}

static size_t
get_irep_record_size(mrb_state *mrb, mrb_irep *irep)
{
  size_t size = 0;
  int irep_no;

  size = get_irep_record_size_1(mrb, irep);
  for (irep_no = 0; irep_no < irep->rlen; irep_no++) {
    size += get_irep_record_size(mrb, irep->reps[irep_no]);
  }
  return size;
}

static int
write_irep_record(mrb_state *mrb, mrb_irep *irep, uint8_t *bin, size_t *irep_record_size, uint8_t flags)
{
  int i;
  uint8_t *src = bin;

  if (irep == NULL) {
    return MRB_DUMP_INVALID_IREP;
  }

  *irep_record_size = get_irep_record_size_1(mrb, irep);
  if (*irep_record_size == 0) {
    return MRB_DUMP_GENERAL_FAILURE;
  }

  bin += write_irep_header(mrb, irep, bin);
  bin += write_iseq_block(mrb, irep, bin, flags);
  bin += write_pool_block(mrb, irep, bin);
  bin += write_syms_block(mrb, irep, bin);

  for (i = 0; i < irep->rlen; i++) {
    int result;
    size_t rsize;

    result = write_irep_record(mrb, irep->reps[i], bin, &rsize, flags);
    if (result != MRB_DUMP_OK) {
      return result;
    }
    bin += rsize;
  }
  *irep_record_size = bin - src;
  return MRB_DUMP_OK;
}

static uint32_t
write_footer(mrb_state *mrb, uint8_t *bin)
{
  struct rite_binary_footer footer;

  memcpy(footer.section_ident, RITE_BINARY_EOF, sizeof(footer.section_ident));
  uint32_to_bin(sizeof(struct rite_binary_footer), footer.section_size);
  memcpy(bin, &footer, sizeof(struct rite_binary_footer));

  return sizeof(struct rite_binary_footer);
}


static int
write_section_irep_header(mrb_state *mrb, size_t section_size, uint8_t *bin)
{
  struct rite_section_irep_header *header = (struct rite_section_irep_header*)bin;

  memcpy(header->section_ident, RITE_SECTION_IREP_IDENT, sizeof(header->section_ident));

  mrb_assert_int_fit(size_t, section_size, uint32_t, UINT32_MAX);
  uint32_to_bin((uint32_t)section_size, header->section_size);
  memcpy(header->rite_version, RITE_VM_VER, sizeof(header->rite_version));

  return MRB_DUMP_OK;
}

static int
write_section_irep(mrb_state *mrb, mrb_irep *irep, uint8_t *bin, size_t *len_p, uint8_t flags)
{
  int result;
  size_t rsize = 0;
  uint8_t *cur = bin;

  if (mrb == NULL || bin == NULL) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }

  cur += sizeof(struct rite_section_irep_header);

  result = write_irep_record(mrb, irep, cur, &rsize, flags);
  if (result != MRB_DUMP_OK) {
    return result;
  }
  *len_p = cur - bin + rsize;
  write_section_irep_header(mrb, *len_p, bin);

  return MRB_DUMP_OK;
}

static size_t
get_debug_record_size(mrb_state *mrb, mrb_irep *irep)
{
  size_t ret = 0;
  uint16_t f_idx;
  int i;

  ret += sizeof(uint32_t); /* record size */
  ret += sizeof(uint16_t); /* file count */

  for (f_idx = 0; f_idx < irep->debug_info->flen; ++f_idx) {
    mrb_irep_debug_info_file const* file = irep->debug_info->files[f_idx];

    ret += sizeof(uint32_t); /* position */
    ret += sizeof(uint16_t); /* filename index */

    /* lines */
    ret += sizeof(uint32_t); /* entry count */
    ret += sizeof(uint8_t); /* line type */
    switch (file->line_type) {
      case mrb_debug_line_ary:
        ret += sizeof(uint16_t) * (size_t)(file->line_entry_count);
        break;

      case mrb_debug_line_flat_map:
        ret += (sizeof(uint32_t) + sizeof(uint16_t)) * (size_t)(file->line_entry_count);
        break;

      default: mrb_assert(0); break;
    }
  }
  for (i=0; i<irep->rlen; i++) {
    ret += get_debug_record_size(mrb, irep->reps[i]);
  }

  return ret;
}

static int
find_filename_index(const mrb_sym *ary, int ary_len, mrb_sym s)
{
  int i;

  for (i = 0; i < ary_len; ++i) {
    if (ary[i] == s) { return i; }
  }
  return -1;
}

static size_t
get_filename_table_size(mrb_state *mrb, mrb_irep *irep, mrb_sym **fp, uint16_t *lp)
{
  mrb_sym *filenames = *fp;
  size_t size = 0;
  mrb_irep_debug_info *di = irep->debug_info;
  int i;

  mrb_assert(lp);
  for (i = 0; i < di->flen; ++i) {
    mrb_irep_debug_info_file *file;
    mrb_int filename_len;

    file = di->files[i];
    if (find_filename_index(filenames, *lp, file->filename_sym) == -1) {
      /* register filename */
      *lp += 1;
      *fp = filenames = (mrb_sym *)mrb_realloc(mrb, filenames, sizeof(mrb_sym) * (*lp));
      filenames[*lp - 1] = file->filename_sym;

      /* filename */
      mrb_sym_name_len(mrb, file->filename_sym, &filename_len);
      size += sizeof(uint16_t) + (size_t)filename_len;
    }
  }
  for (i=0; i<irep->rlen; i++) {
    size += get_filename_table_size(mrb, irep->reps[i], fp, lp);
  }
  return size;
}

static size_t
write_debug_record_1(mrb_state *mrb, mrb_irep *irep, uint8_t *bin, mrb_sym const* filenames, uint16_t filenames_len)
{
  uint8_t *cur;
  uint16_t f_idx;
  ptrdiff_t ret;

  cur = bin + sizeof(uint32_t); /* skip record size */
  cur += uint16_to_bin(irep->debug_info->flen, cur); /* file count */

  for (f_idx = 0; f_idx < irep->debug_info->flen; ++f_idx) {
    int filename_idx;
    const mrb_irep_debug_info_file *file = irep->debug_info->files[f_idx];

    /* position */
    cur += uint32_to_bin(file->start_pos, cur);

    /* filename index */
    filename_idx = find_filename_index(filenames, filenames_len,
                                                  file->filename_sym);
    mrb_assert_int_fit(int, filename_idx, uint16_t, UINT16_MAX);
    cur += uint16_to_bin((uint16_t)filename_idx, cur);

    /* lines */
    cur += uint32_to_bin(file->line_entry_count, cur);
    cur += uint8_to_bin(file->line_type, cur);
    switch (file->line_type) {
      case mrb_debug_line_ary: {
        uint32_t l;
        for (l = 0; l < file->line_entry_count; ++l) {
          cur += uint16_to_bin(file->lines.ary[l], cur);
        }
      } break;

      case mrb_debug_line_flat_map: {
        uint32_t line;
        for (line = 0; line < file->line_entry_count; ++line) {
          cur += uint32_to_bin(file->lines.flat_map[line].start_pos, cur);
          cur += uint16_to_bin(file->lines.flat_map[line].line, cur);
        }
      } break;

      default: mrb_assert(0); break;
    }
  }

  ret = cur - bin;
  mrb_assert_int_fit(ptrdiff_t, ret, uint32_t, UINT32_MAX);
  uint32_to_bin((uint32_t)ret, bin);

  mrb_assert_int_fit(ptrdiff_t, ret, size_t, SIZE_MAX);
  return (size_t)ret;
}

static size_t
write_debug_record(mrb_state *mrb, mrb_irep *irep, uint8_t *bin, mrb_sym const* filenames, uint16_t filenames_len)
{
  size_t size, len;
  int irep_no;

  size = len = write_debug_record_1(mrb, irep, bin, filenames, filenames_len);
  bin += len;
  for (irep_no = 0; irep_no < irep->rlen; irep_no++) {
    len = write_debug_record(mrb, irep->reps[irep_no], bin, filenames, filenames_len);
    bin += len;
    size += len;
  }

  mrb_assert(size == get_debug_record_size(mrb, irep));
  return size;
}

static int
write_section_debug(mrb_state *mrb, mrb_irep *irep, uint8_t *cur, mrb_sym const *filenames, uint16_t filenames_len)
{
  size_t section_size = 0;
  const uint8_t *bin = cur;
  struct rite_section_debug_header *header;
  size_t dlen;
  uint16_t i;
  char const *sym; mrb_int sym_len;

  if (mrb == NULL || cur == NULL) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }

  header = (struct rite_section_debug_header *)bin;
  cur += sizeof(struct rite_section_debug_header);
  section_size += sizeof(struct rite_section_debug_header);

  /* filename table */
  cur += uint16_to_bin(filenames_len, cur);
  section_size += sizeof(uint16_t);
  for (i = 0; i < filenames_len; ++i) {
    sym = mrb_sym_name_len(mrb, filenames[i], &sym_len);
    mrb_assert(sym);
    cur += uint16_to_bin((uint16_t)sym_len, cur);
    memcpy(cur, sym, sym_len);
    cur += sym_len;
    section_size += sizeof(uint16_t) + sym_len;
  }

  /* debug records */
  dlen = write_debug_record(mrb, irep, cur, filenames, filenames_len);
  section_size += dlen;

  memcpy(header->section_ident, RITE_SECTION_DEBUG_IDENT, sizeof(header->section_ident));
  mrb_assert(section_size <= INT32_MAX);
  uint32_to_bin((uint32_t)section_size, header->section_size);

  return MRB_DUMP_OK;
}

static void
create_lv_sym_table(mrb_state *mrb, const mrb_irep *irep, mrb_sym **syms, uint32_t *syms_len)
{
  int i;

  if (*syms == NULL) {
    *syms = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym) * 1);
  }

  for (i = 0; i + 1 < irep->nlocals; ++i) {
    mrb_sym const name = irep->lv[i].name;
    if (name == 0) continue;
    if (find_filename_index(*syms, *syms_len, name) != -1) continue;

    ++(*syms_len);
    *syms = (mrb_sym*)mrb_realloc(mrb, *syms, sizeof(mrb_sym) * (*syms_len));
    (*syms)[*syms_len - 1] = name;
  }

  for (i = 0; i < irep->rlen; ++i) {
    create_lv_sym_table(mrb, irep->reps[i], syms, syms_len);
  }
}

static int
write_lv_sym_table(mrb_state *mrb, uint8_t **start, mrb_sym const *syms, uint32_t syms_len)
{
  uint8_t *cur = *start;
  uint32_t i;
  const char *str;
  mrb_int str_len;

  cur += uint32_to_bin(syms_len, cur);

  for (i = 0; i < syms_len; ++i) {
    str = mrb_sym_name_len(mrb, syms[i], &str_len);
    cur += uint16_to_bin((uint16_t)str_len, cur);
    memcpy(cur, str, str_len);
    cur += str_len;
  }

  *start = cur;

  return MRB_DUMP_OK;
}

static int
write_lv_record(mrb_state *mrb, const mrb_irep *irep, uint8_t **start, mrb_sym const *syms, uint32_t syms_len)
{
  uint8_t *cur = *start;
  int i;

  for (i = 0; i + 1 < irep->nlocals; ++i) {
    if (irep->lv[i].name == 0) {
      cur += uint16_to_bin(RITE_LV_NULL_MARK, cur);
      cur += uint16_to_bin(0, cur);
    }
    else {
      int const sym_idx = find_filename_index(syms, syms_len, irep->lv[i].name);
      mrb_assert(sym_idx != -1); /* local variable name must be in syms */

      cur += uint16_to_bin(sym_idx, cur);
      cur += uint16_to_bin(irep->lv[i].r, cur);
    }
  }

  for (i = 0; i < irep->rlen; ++i) {
    write_lv_record(mrb, irep->reps[i], &cur, syms, syms_len);
  }

  *start = cur;

  return MRB_DUMP_OK;
}

static size_t
get_lv_record_size(mrb_state *mrb, mrb_irep *irep)
{
  size_t ret = 0;
  int i;

  ret += (sizeof(uint16_t) + sizeof(uint16_t)) * (irep->nlocals - 1);

  for (i = 0; i < irep->rlen; ++i) {
    ret += get_lv_record_size(mrb, irep->reps[i]);
  }

  return ret;
}

static size_t
get_lv_section_size(mrb_state *mrb, mrb_irep *irep, mrb_sym const *syms, uint32_t syms_len)
{
  size_t ret = 0, i;

  ret += sizeof(uint32_t); /* syms_len */
  ret += sizeof(uint16_t) * syms_len; /* symbol name lengths */
  for (i = 0; i < syms_len; ++i) {
    mrb_int str_len;
    mrb_sym_name_len(mrb, syms[i], &str_len);
    ret += str_len;
  }

  ret += get_lv_record_size(mrb, irep);

  return ret;
}

static int
write_section_lv(mrb_state *mrb, mrb_irep *irep, uint8_t *start, mrb_sym const *syms, uint32_t const syms_len)
{
  uint8_t *cur = start;
  struct rite_section_lv_header *header;
  ptrdiff_t diff;
  int result = MRB_DUMP_OK;

  if (mrb == NULL || cur == NULL) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }

  header = (struct rite_section_lv_header*)cur;
  cur += sizeof(struct rite_section_lv_header);

  result = write_lv_sym_table(mrb, &cur, syms, syms_len);
  if (result != MRB_DUMP_OK) {
    goto lv_section_exit;
  }

  result = write_lv_record(mrb, irep, &cur, syms, syms_len);
  if (result != MRB_DUMP_OK) {
    goto lv_section_exit;
  }

  memcpy(header->section_ident, RITE_SECTION_LV_IDENT, sizeof(header->section_ident));

  diff = cur - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  uint32_to_bin((uint32_t)diff, header->section_size);

lv_section_exit:
  return result;
}

static int
write_rite_binary_header(mrb_state *mrb, size_t binary_size, uint8_t *bin, uint8_t flags)
{
  struct rite_binary_header *header = (struct rite_binary_header *)bin;
  uint16_t crc;
  uint32_t offset;

  memcpy(header->binary_ident, RITE_BINARY_IDENT, sizeof(header->binary_ident));
  memcpy(header->binary_version, RITE_BINARY_FORMAT_VER, sizeof(header->binary_version));
  memcpy(header->compiler_name, RITE_COMPILER_NAME, sizeof(header->compiler_name));
  memcpy(header->compiler_version, RITE_COMPILER_VERSION, sizeof(header->compiler_version));
  mrb_assert(binary_size <= UINT32_MAX);
  uint32_to_bin((uint32_t)binary_size, header->binary_size);

  offset = (uint32_t)((&(header->binary_crc[0]) - bin) + sizeof(uint16_t));
  crc = calc_crc_16_ccitt(bin + offset, binary_size - offset, 0);
  uint16_to_bin(crc, header->binary_crc);

  return MRB_DUMP_OK;
}

static mrb_bool
debug_info_defined_p(mrb_irep *irep)
{
  int i;

  if (!irep->debug_info) return FALSE;
  for (i=0; i<irep->rlen; i++) {
    if (!debug_info_defined_p(irep->reps[i])) return FALSE;
  }
  return TRUE;
}

static mrb_bool
lv_defined_p(mrb_irep *irep)
{
  int i;

  if (irep->lv) { return TRUE; }

  for (i = 0; i < irep->rlen; ++i) {
    if (lv_defined_p(irep->reps[i])) { return TRUE; }
  }

  return FALSE;
}

static int
dump_irep(mrb_state *mrb, mrb_irep *irep, uint8_t flags, uint8_t **bin, size_t *bin_size)
{
  int result = MRB_DUMP_GENERAL_FAILURE;
  size_t malloc_size;
  size_t section_irep_size;
  size_t section_lineno_size = 0, section_lv_size = 0;
  uint8_t *cur = NULL;
  mrb_bool const debug_info_defined = debug_info_defined_p(irep), lv_defined = lv_defined_p(irep);
  mrb_sym *lv_syms = NULL; uint32_t lv_syms_len = 0;
  mrb_sym *filenames = NULL; uint16_t filenames_len = 0;

  if (mrb == NULL) {
    *bin = NULL;
    return MRB_DUMP_GENERAL_FAILURE;
  }

  section_irep_size = sizeof(struct rite_section_irep_header);
  section_irep_size += get_irep_record_size(mrb, irep);

  /* DEBUG section size */
  if (flags & DUMP_DEBUG_INFO) {
    if (debug_info_defined) {
      section_lineno_size += sizeof(struct rite_section_debug_header);
      /* filename table */
      filenames = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym) + 1);

      /* filename table size */
      section_lineno_size += sizeof(uint16_t);
      section_lineno_size += get_filename_table_size(mrb, irep, &filenames, &filenames_len);

      section_lineno_size += get_debug_record_size(mrb, irep);
    }
  }

  if (lv_defined) {
    section_lv_size += sizeof(struct rite_section_lv_header);
    create_lv_sym_table(mrb, irep, &lv_syms, &lv_syms_len);
    section_lv_size += get_lv_section_size(mrb, irep, lv_syms, lv_syms_len);
  }

  malloc_size = sizeof(struct rite_binary_header) +
                section_irep_size + section_lineno_size + section_lv_size +
                sizeof(struct rite_binary_footer);
  cur = *bin = (uint8_t*)mrb_malloc(mrb, malloc_size);
  cur += sizeof(struct rite_binary_header);

  result = write_section_irep(mrb, irep, cur, &section_irep_size, flags);
  if (result != MRB_DUMP_OK) {
    goto error_exit;
  }
  cur += section_irep_size;
  *bin_size = sizeof(struct rite_binary_header) +
              section_irep_size + section_lineno_size + section_lv_size +
              sizeof(struct rite_binary_footer);

  /* write DEBUG section */
  if (flags & DUMP_DEBUG_INFO) {
    if (debug_info_defined) {
      result = write_section_debug(mrb, irep, cur, filenames, filenames_len);
      if (result != MRB_DUMP_OK) {
        goto error_exit;
      }
    }
    cur += section_lineno_size;
  }

  if (lv_defined) {
    result = write_section_lv(mrb, irep, cur, lv_syms, lv_syms_len);
    if (result != MRB_DUMP_OK) {
      goto error_exit;
    }
    cur += section_lv_size;
  }

  write_footer(mrb, cur);
  write_rite_binary_header(mrb, *bin_size, *bin, flags);

error_exit:
  if (result != MRB_DUMP_OK) {
    mrb_free(mrb, *bin);
    *bin = NULL;
  }
  mrb_free(mrb, lv_syms);
  mrb_free(mrb, filenames);
  return result;
}

int
mrb_dump_irep(mrb_state *mrb, mrb_irep *irep, uint8_t flags, uint8_t **bin, size_t *bin_size)
{
  return dump_irep(mrb, irep, flags, bin, bin_size);
}

#ifndef MRB_DISABLE_STDIO

int
mrb_dump_irep_binary(mrb_state *mrb, mrb_irep *irep, uint8_t flags, FILE* fp)
{
  uint8_t *bin = NULL;
  size_t bin_size = 0;
  int result;

  if (fp == NULL) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }

  result = dump_irep(mrb, irep, flags, &bin, &bin_size);
  if (result == MRB_DUMP_OK) {
    if (fwrite(bin, sizeof(bin[0]), bin_size, fp) != bin_size) {
      result = MRB_DUMP_WRITE_FAULT;
    }
  }

  mrb_free(mrb, bin);
  return result;
}

int
mrb_dump_irep_cfunc(mrb_state *mrb, mrb_irep *irep, uint8_t flags, FILE *fp, const char *initname)
{
  uint8_t *bin = NULL;
  size_t bin_size = 0, bin_idx = 0;
  int result;

  if (fp == NULL || initname == NULL || initname[0] == '\0') {
    return MRB_DUMP_INVALID_ARGUMENT;
  }
  result = dump_irep(mrb, irep, flags, &bin, &bin_size);
  if (result == MRB_DUMP_OK) {
    if (fprintf(fp, "#include <stdint.h>\n") < 0) { /* for uint8_t under at least Darwin */
      mrb_free(mrb, bin);
      return MRB_DUMP_WRITE_FAULT;
    }
    if (fprintf(fp,
          "#ifdef __cplusplus\n"
          "extern const uint8_t %s[];\n"
          "#endif\n"
          "const uint8_t\n"
          "#if defined __GNUC__\n"
          "__attribute__((aligned(%u)))\n"
          "#elif defined _MSC_VER\n"
          "__declspec(align(%u))\n"
          "#endif\n"
          "%s[] = {",
          initname,
          (uint16_t)MRB_DUMP_ALIGNMENT, (uint16_t)MRB_DUMP_ALIGNMENT, initname) < 0) {
      mrb_free(mrb, bin);
      return MRB_DUMP_WRITE_FAULT;
    }
    while (bin_idx < bin_size) {
      if (bin_idx % 16 == 0) {
        if (fputs("\n", fp) == EOF) {
          mrb_free(mrb, bin);
          return MRB_DUMP_WRITE_FAULT;
        }
      }
      if (fprintf(fp, "0x%02x,", bin[bin_idx++]) < 0) {
        mrb_free(mrb, bin);
        return MRB_DUMP_WRITE_FAULT;
      }
    }
    if (fputs("\n};\n", fp) == EOF) {
      mrb_free(mrb, bin);
      return MRB_DUMP_WRITE_FAULT;
    }
  }

  mrb_free(mrb, bin);
  return result;
}

#endif /* MRB_DISABLE_STDIO */
/*
** enum.c - Enumerable module
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/proc.h>

/* internal method `__update_hash(oldhash, index, itemhash)` */
static mrb_value
enum_update_hash(mrb_state *mrb, mrb_value self)
{
  mrb_int hash;
  mrb_int index;
  mrb_int hv;

  mrb_get_args(mrb, "iii", &hash, &index, &hv);
  hash ^= ((uint32_t)hv << (index % 16));

  return mrb_fixnum_value(hash);
}

void
mrb_init_enumerable(mrb_state *mrb)
{
  struct RClass *enumerable;
  enumerable = mrb_define_module(mrb, "Enumerable");  /* 15.3.2 */
  mrb_define_module_function(mrb, enumerable, "__update_hash", enum_update_hash, MRB_ARGS_REQ(3));
}
/*
** error.c - Exception class
**
** See Copyright Notice in mruby.h
*/

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/class.h>
#include <mruby/throw.h>

MRB_API mrb_value
mrb_exc_new(mrb_state *mrb, struct RClass *c, const char *ptr, size_t len)
{
  mrb_value arg = mrb_str_new(mrb, ptr, len);
  return mrb_obj_new(mrb, c, 1, &arg);
}

MRB_API mrb_value
mrb_exc_new_str(mrb_state *mrb, struct RClass* c, mrb_value str)
{
  mrb_to_str(mrb, str);
  return mrb_obj_new(mrb, c, 1, &str);
}

/*
 * call-seq:
 *    Exception.new(msg = nil)   ->  exception
 *
 *  Construct a new Exception object, optionally passing in
 *  a message.
 */

static mrb_value
exc_initialize(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg;

  if (mrb_get_args(mrb, "|o", &mesg) == 1) {
    mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "mesg"), mesg);
  }
  return exc;
}

/*
 *  Document-method: exception
 *
 *  call-seq:
 *     exc.exception(string)  ->  an_exception or exc
 *
 *  With no argument, or if the argument is the same as the receiver,
 *  return the receiver. Otherwise, create a new
 *  exception object of the same class as the receiver, but with a
 *  message equal to <code>string</code>.
 *
 */

static mrb_value
exc_exception(mrb_state *mrb, mrb_value self)
{
  mrb_value exc;
  mrb_value a;
  mrb_int argc;

  argc = mrb_get_args(mrb, "|o", &a);
  if (argc == 0) return self;
  if (mrb_obj_equal(mrb, self, a)) return self;
  exc = mrb_obj_clone(mrb, self);
  mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "mesg"), a);

  return exc;
}

/*
 * call-seq:
 *   exception.to_s   ->  string
 *
 * Returns exception's message (or the name of the exception if
 * no message is set).
 */

mrb_value
exc_to_s(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg = mrb_attr_get(mrb, exc, mrb_intern_lit(mrb, "mesg"));
  struct RObject *p;

  if (!mrb_string_p(mesg)) {
    return mrb_str_new_cstr(mrb, mrb_obj_classname(mrb, exc));
  }
  p = mrb_obj_ptr(mesg);
  if (!p->c) {
    p->c = mrb->string_class;
  }
  return mesg;
}

/*
 * call-seq:
 *   exception.message   ->  string
 *
 * Returns the result of invoking <code>exception.to_s</code>.
 * Normally this returns the exception's message or name.
 */

static mrb_value
exc_message(mrb_state *mrb, mrb_value exc)
{
  return mrb_funcall(mrb, exc, "to_s", 0);
}

/*
 * call-seq:
 *   exception.inspect   -> string
 *
 * Returns this exception's file name, line number,
 * message and class name.
 * If file name or line number is not set,
 * returns message and class name.
 */

mrb_value
mrb_exc_inspect(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg = mrb_attr_get(mrb, exc, mrb_intern_lit(mrb, "mesg"));
  mrb_value cname = mrb_mod_to_s(mrb, mrb_obj_value(mrb_obj_class(mrb, exc)));
  mesg = mrb_obj_as_string(mrb, mesg);
  return RSTRING_LEN(mesg) == 0 ? cname : mrb_format(mrb, "%v (%v)", mesg, cname);
}

void mrb_keep_backtrace(mrb_state *mrb, mrb_value exc);

static void
set_backtrace(mrb_state *mrb, mrb_value exc, mrb_value backtrace)
{
  if (!mrb_array_p(backtrace)) {
  type_err:
    mrb_raise(mrb, E_TYPE_ERROR, "backtrace must be Array of String");
  }
  else {
    const mrb_value *p = RARRAY_PTR(backtrace);
    const mrb_value *pend = p + RARRAY_LEN(backtrace);

    while (p < pend) {
      if (!mrb_string_p(*p)) goto type_err;
      p++;
    }
  }
  mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "backtrace"), backtrace);
}

static mrb_value
exc_set_backtrace(mrb_state *mrb, mrb_value exc)
{
  mrb_value backtrace = mrb_get_arg1(mrb);

  set_backtrace(mrb, exc, backtrace);
  return backtrace;
}

void
mrb_exc_set(mrb_state *mrb, mrb_value exc)
{
  if (mrb_nil_p(exc)) {
    mrb->exc = 0;
  }
  else {
    mrb->exc = mrb_obj_ptr(exc);
    if (mrb->gc.arena_idx > 0 &&
        (struct RBasic*)mrb->exc == mrb->gc.arena[mrb->gc.arena_idx-1]) {
      mrb->gc.arena_idx--;
    }
    if (!mrb->gc.out_of_memory && !mrb_frozen_p(mrb->exc)) {
      mrb_keep_backtrace(mrb, exc);
    }
  }
}

static mrb_noreturn void
exc_throw(mrb_state *mrb, mrb_value exc)
{
  if (!mrb->jmp) {
    mrb_p(mrb, exc);
    abort();
  }
  MRB_THROW(mrb->jmp);
}

MRB_API mrb_noreturn void
mrb_exc_raise(mrb_state *mrb, mrb_value exc)
{
  if (mrb_break_p(exc)) {
    mrb->exc = mrb_obj_ptr(exc);
  }
  else {
    if (!mrb_obj_is_kind_of(mrb, exc, mrb->eException_class)) {
      mrb_raise(mrb, E_TYPE_ERROR, "exception object expected");
    }
    mrb_exc_set(mrb, exc);
  }
  exc_throw(mrb, exc);
}

MRB_API mrb_noreturn void
mrb_raise(mrb_state *mrb, struct RClass *c, const char *msg)
{
  mrb_exc_raise(mrb, mrb_exc_new_str(mrb, c, mrb_str_new_cstr(mrb, msg)));
}

/*
 * <code>vsprintf</code> like formatting.
 *
 * The syntax of a format sequence is as follows.
 *
 *   %[modifier]specifier
 *
 * The modifiers are:
 *
 *   ----------+------------------------------------------------------------
 *   Modifier  | Meaning
 *   ----------+------------------------------------------------------------
 *       !     | Convert to string by corresponding `inspect` instead of
 *             | corresponding `to_s`.
 *   ----------+------------------------------------------------------------
 *
 * The specifiers are:
 *
 *   ----------+----------------+--------------------------------------------
 *   Specifier | Argument Type  | Note
 *   ----------+----------------+--------------------------------------------
 *       c     | char           |
 *       d     | int            |
 *       f     | mrb_float      |
 *       i     | mrb_int        |
 *       l     | char*, size_t  | Arguments are string and length.
 *       n     | mrb_sym        |
 *       s     | char*          | Argument is NUL terminated string.
 *       t     | mrb_value      | Convert to type (class) of object.
 *      v,S    | mrb_value      |
 *       C     | struct RClass* |
 *       T     | mrb_value      | Convert to real type (class) of object.
 *       Y     | mrb_value      | Same as `!v` if argument is `true`, `false`
 *             |                | or `nil`, otherwise same as `T`.
 *       %     | -              | Convert to percent sign itself (no argument
 *             |                | taken).
 *   ----------+----------------+--------------------------------------------
 */
MRB_API mrb_value
mrb_vformat(mrb_state *mrb, const char *format, va_list ap)
{
  const char *chars, *p = format, *b = format, *e;
  char ch;
  size_t len;
  mrb_int i;
  struct RClass *cls;
  mrb_bool inspect = FALSE;
  mrb_value result = mrb_str_new_capa(mrb, 128), obj, str;
  int ai = mrb_gc_arena_save(mrb);

  while (*p) {
    const char c = *p++;
    e = p;
    if (c == '%') {
      if (*p == '!') {
        inspect = TRUE;
        ++p;
      }
      if (!*p) break;
      switch (*p) {
        case 'c':
          ch = (char)va_arg(ap, int);
          chars = &ch;
          len = 1;
          goto L_cat;
        case 'd': case 'i':
#if MRB_INT_MAX < INT_MAX
          i = (mrb_int)va_arg(ap, int);
#else
          i = *p == 'd' ? (mrb_int)va_arg(ap, int) : va_arg(ap, mrb_int);
#endif
          obj = mrb_fixnum_value(i);
          goto L_cat_obj;
#ifndef MRB_WITHOUT_FLOAT
        case 'f':
          obj = mrb_float_value(mrb, (mrb_float)va_arg(ap, double));
          goto L_cat_obj;
#endif
        case 'l':
          chars = va_arg(ap, char*);
          len = va_arg(ap, size_t);
        L_cat:
          if (inspect) {
            obj = mrb_str_new(mrb, chars, len);
            goto L_cat_obj;
          }
          mrb_str_cat(mrb, result, b,  e - b - 1);
          mrb_str_cat(mrb, result, chars, len);
          b = ++p;
          mrb_gc_arena_restore(mrb, ai);
          break;
        case 'n':
#if UINT32_MAX < INT_MAX
          obj = mrb_symbol_value((mrb_sym)va_arg(ap, int));
#else
          obj = mrb_symbol_value(va_arg(ap, mrb_sym));
#endif
          goto L_cat_obj;
        case 's':
          chars = va_arg(ap, char*);
          len = strlen(chars);
          goto L_cat;
        case 't':
          cls = mrb_class(mrb, va_arg(ap, mrb_value));
          goto L_cat_class;
        case 'v': case 'S':
          obj = va_arg(ap, mrb_value);
        L_cat_obj:
          str = (inspect ? mrb_inspect : mrb_obj_as_string)(mrb, obj);
          chars = RSTRING_PTR(str);
          len = RSTRING_LEN(str);
          inspect = FALSE;
          goto L_cat;
        case 'C':
          cls = va_arg(ap, struct RClass*);
        L_cat_class:
          obj = mrb_obj_value(cls);
          goto L_cat_obj;
        case 'T':
          obj = va_arg(ap, mrb_value);
        L_cat_real_class_of:
          cls = mrb_obj_class(mrb, obj);
          goto L_cat_class;
        case 'Y':
          obj = va_arg(ap, mrb_value);
          if (!mrb_test(obj) || mrb_true_p(obj)) {
            inspect = TRUE;
            goto L_cat_obj;
          }
          else {
            goto L_cat_real_class_of;
          }
        case '%':
        L_cat_current:
          chars = p;
          len = 1;
          goto L_cat;
        default:
          mrb_raisef(mrb, E_ARGUMENT_ERROR, "malformed format string - %%%c", *p);
      }
    }
    else if (c == '\\') {
      if (!*p) break;
      goto L_cat_current;

    }
  }

  mrb_str_cat(mrb, result, b, p - b);
  return result;
}

MRB_API mrb_value
mrb_format(mrb_state *mrb, const char *format, ...)
{
  va_list ap;
  mrb_value str;

  va_start(ap, format);
  str = mrb_vformat(mrb, format, ap);
  va_end(ap);

  return str;
}

static mrb_noreturn void
raise_va(mrb_state *mrb, struct RClass *c, const char *fmt, va_list ap, int argc, mrb_value *argv)
{
  mrb_value mesg;

  mesg = mrb_vformat(mrb, fmt, ap);
  if (argv == NULL) {
    argv = &mesg;
  }
  else {
    argv[0] = mesg;
  }
  mrb_exc_raise(mrb, mrb_obj_new(mrb, c, argc+1, argv));
}

MRB_API mrb_noreturn void
mrb_raisef(mrb_state *mrb, struct RClass *c, const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  raise_va(mrb, c, fmt, args, 0, NULL);
  va_end(args);
}

MRB_API mrb_noreturn void
mrb_name_error(mrb_state *mrb, mrb_sym id, const char *fmt, ...)
{
  mrb_value argv[2];
  va_list args;

  va_start(args, fmt);
  argv[1] = mrb_symbol_value(id);
  raise_va(mrb, E_NAME_ERROR, fmt, args, 1, argv);
  va_end(args);
}

MRB_API void
mrb_warn(mrb_state *mrb, const char *fmt, ...)
{
#ifndef MRB_DISABLE_STDIO
  va_list ap;
  mrb_value str;

  va_start(ap, fmt);
  str = mrb_vformat(mrb, fmt, ap);
  fputs("warning: ", stderr);
  fwrite(RSTRING_PTR(str), RSTRING_LEN(str), 1, stderr);
  putc('\n', stderr);
  va_end(ap);
#endif
}

MRB_API mrb_noreturn void
mrb_bug(mrb_state *mrb, const char *fmt, ...)
{
#ifndef MRB_DISABLE_STDIO
  va_list ap;
  mrb_value str;

  va_start(ap, fmt);
  str = mrb_vformat(mrb, fmt, ap);
  fputs("bug: ", stderr);
  fwrite(RSTRING_PTR(str), RSTRING_LEN(str), 1, stderr);
  va_end(ap);
#endif
  exit(EXIT_FAILURE);
}

MRB_API mrb_value
mrb_make_exception(mrb_state *mrb, mrb_int argc, const mrb_value *argv)
{
  mrb_value mesg;
  int n;

  mesg = mrb_nil_value();
  switch (argc) {
    case 0:
    break;
    case 1:
      if (mrb_nil_p(argv[0]))
        break;
      if (mrb_string_p(argv[0])) {
        mesg = mrb_exc_new_str(mrb, E_RUNTIME_ERROR, argv[0]);
        break;
      }
      n = 0;
      goto exception_call;

    case 2:
    case 3:
      n = 1;
exception_call:
      {
        mrb_sym exc = mrb_intern_lit(mrb, "exception");
        if (mrb_respond_to(mrb, argv[0], exc)) {
          mesg = mrb_funcall_argv(mrb, argv[0], exc, n, argv+1);
        }
        else {
          /* undef */
          mrb_raise(mrb, E_TYPE_ERROR, "exception class/object expected");
        }
      }

      break;
    default:
      mrb_argnum_error(mrb, argc, 0, 3);
      break;
  }
  if (argc > 0) {
    if (!mrb_obj_is_kind_of(mrb, mesg, mrb->eException_class))
      mrb_raise(mrb, mrb->eException_class, "exception object expected");
    if (argc > 2)
      set_backtrace(mrb, mesg, argv[2]);
  }

  return mesg;
}

MRB_API void
mrb_sys_fail(mrb_state *mrb, const char *mesg)
{
  struct RClass *sce;
  mrb_int no;

  no = (mrb_int)errno;
  if (mrb_class_defined(mrb, "SystemCallError")) {
    sce = mrb_class_get(mrb, "SystemCallError");
    if (mesg != NULL) {
      mrb_funcall(mrb, mrb_obj_value(sce), "_sys_fail", 2, mrb_fixnum_value(no), mrb_str_new_cstr(mrb, mesg));
    }
    else {
      mrb_funcall(mrb, mrb_obj_value(sce), "_sys_fail", 1, mrb_fixnum_value(no));
    }
  }
  else {
    mrb_raise(mrb, E_RUNTIME_ERROR, mesg);
  }
}

MRB_API mrb_noreturn void
mrb_no_method_error(mrb_state *mrb, mrb_sym id, mrb_value args, char const* fmt, ...)
{
  mrb_value exc;
  mrb_value argv[3];
  va_list ap;

  va_start(ap, fmt);
  argv[0] = mrb_vformat(mrb, fmt, ap);
  argv[1] = mrb_symbol_value(id);
  argv[2] = args;
  va_end(ap);
  exc = mrb_obj_new(mrb, E_NOMETHOD_ERROR, 3, argv);
  mrb_exc_raise(mrb, exc);
}

MRB_API mrb_noreturn void
mrb_frozen_error(mrb_state *mrb, void *frozen_obj)
{
  mrb_raisef(mrb, E_FROZEN_ERROR, "can't modify frozen %t", mrb_obj_value(frozen_obj));
}

MRB_API mrb_noreturn void
mrb_argnum_error(mrb_state *mrb, mrb_int argc, int min, int max)
{
#define FMT(exp) "wrong number of arguments (given %i, expected " exp ")"
  if (min == max)
    mrb_raisef(mrb, E_ARGUMENT_ERROR, FMT("%d"), argc, min);
  else if (max < 0)
    mrb_raisef(mrb, E_ARGUMENT_ERROR, FMT("%d+"), argc, min);
  else
    mrb_raisef(mrb, E_ARGUMENT_ERROR, FMT("%d..%d"), argc, min, max);
#undef FMT
}

void mrb_core_init_printabort(void);

int
mrb_core_init_protect(mrb_state *mrb, void (*body)(mrb_state *, void *), void *opaque)
{
  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  int err = 1;

  MRB_TRY(&c_jmp) {
    mrb->jmp = &c_jmp;
    body(mrb, opaque);
    err = 0;
  } MRB_CATCH(&c_jmp) {
    if (mrb->exc) {
      mrb_p(mrb, mrb_obj_value(mrb->exc));
      mrb->exc = NULL;
    }
    else {
      mrb_core_init_printabort();
    }
  } MRB_END_EXC(&c_jmp);

  mrb->jmp = prev_jmp;

  return err;
}

mrb_noreturn void
mrb_core_init_abort(mrb_state *mrb)
{
  mrb->exc = NULL;
  exc_throw(mrb, mrb_nil_value());
}

mrb_noreturn void
mrb_raise_nomemory(mrb_state *mrb)
{
  if (mrb->nomem_err) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->nomem_err));
  }
  else {
    mrb_core_init_abort(mrb);
  }
}

void
mrb_init_exception(mrb_state *mrb)
{
  struct RClass *exception, *script_error, *stack_error, *nomem_error;

  mrb->eException_class = exception = mrb_define_class(mrb, "Exception", mrb->object_class); /* 15.2.22 */
  MRB_SET_INSTANCE_TT(exception, MRB_TT_EXCEPTION);
  mrb_define_class_method(mrb, exception, "exception", mrb_instance_new,  MRB_ARGS_OPT(1));
  mrb_define_method(mrb, exception, "exception",       exc_exception,     MRB_ARGS_OPT(1));
  mrb_define_method(mrb, exception, "initialize",      exc_initialize,    MRB_ARGS_OPT(1));
  mrb_define_method(mrb, exception, "to_s",            exc_to_s,          MRB_ARGS_NONE());
  mrb_define_method(mrb, exception, "message",         exc_message,       MRB_ARGS_NONE());
  mrb_define_method(mrb, exception, "inspect",         mrb_exc_inspect,   MRB_ARGS_NONE());
  mrb_define_method(mrb, exception, "backtrace",       mrb_exc_backtrace, MRB_ARGS_NONE());
  mrb_define_method(mrb, exception, "set_backtrace",   exc_set_backtrace, MRB_ARGS_REQ(1));

  mrb->eStandardError_class = mrb_define_class(mrb, "StandardError", mrb->eException_class); /* 15.2.23 */
  mrb_define_class(mrb, "RuntimeError", mrb->eStandardError_class);          /* 15.2.28 */
  script_error = mrb_define_class(mrb, "ScriptError", mrb->eException_class);                /* 15.2.37 */
  mrb_define_class(mrb, "SyntaxError", script_error);                                        /* 15.2.38 */
  stack_error = mrb_define_class(mrb, "SystemStackError", exception);
  mrb->stack_err = mrb_obj_ptr(mrb_exc_new_str_lit(mrb, stack_error, "stack level too deep"));

  nomem_error = mrb_define_class(mrb, "NoMemoryError", exception);
  mrb->nomem_err = mrb_obj_ptr(mrb_exc_new_str_lit(mrb, nomem_error, "Out of memory"));
#ifdef MRB_GC_FIXED_ARENA
  mrb->arena_err = mrb_obj_ptr(mrb_exc_new_str_lit(mrb, nomem_error, "arena overflow error"));
#endif
}
/*
** etc.c
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/string.h>
#include <mruby/data.h>
#include <mruby/class.h>

MRB_API struct RData*
mrb_data_object_alloc(mrb_state *mrb, struct RClass *klass, void *ptr, const mrb_data_type *type)
{
  struct RData *data;

  data = (struct RData*)mrb_obj_alloc(mrb, MRB_TT_DATA, klass);
  data->data = ptr;
  data->type = type;

  return data;
}

MRB_API void
mrb_data_check_type(mrb_state *mrb, mrb_value obj, const mrb_data_type *type)
{
  if (!mrb_data_p(obj)) {
    mrb_check_type(mrb, obj, MRB_TT_DATA);
  }
  if (DATA_TYPE(obj) != type) {
    const mrb_data_type *t2 = DATA_TYPE(obj);

    if (t2) {
      mrb_raisef(mrb, E_TYPE_ERROR, "wrong argument type %s (expected %s)",
                 t2->struct_name, type->struct_name);
    }
    else {
      mrb_raisef(mrb, E_TYPE_ERROR, "uninitialized %t (expected %s)",
                 obj, type->struct_name);
    }
  }
}

MRB_API void*
mrb_data_check_get_ptr(mrb_state *mrb, mrb_value obj, const mrb_data_type *type)
{
  if (!mrb_data_p(obj)) {
    return NULL;
  }
  if (DATA_TYPE(obj) != type) {
    return NULL;
  }
  return DATA_PTR(obj);
}

MRB_API void*
mrb_data_get_ptr(mrb_state *mrb, mrb_value obj, const mrb_data_type *type)
{
  mrb_data_check_type(mrb, obj, type);
  return DATA_PTR(obj);
}

MRB_API mrb_sym
mrb_obj_to_sym(mrb_state *mrb, mrb_value name)
{
  if (mrb_symbol_p(name)) return mrb_symbol(name);
  if (mrb_string_p(name)) return mrb_intern_str(mrb, name);
  mrb_raisef(mrb, E_TYPE_ERROR, "%!v is not a symbol nor a string", name);
  return 0;  /* not reached */
}

MRB_API mrb_int
#ifdef MRB_WITHOUT_FLOAT
mrb_fixnum_id(mrb_int f)
#else
mrb_float_id(mrb_float f)
#endif
{
  const char *p = (const char*)&f;
  int len = sizeof(f);
  uint32_t id = 0;

#ifndef MRB_WITHOUT_FLOAT
  /* normalize -0.0 to 0.0 */
  if (f == 0) f = 0.0;
#endif
  while (len--) {
    id = id*65599 + *p;
    p++;
  }
  id = id + (id>>5);

  return (mrb_int)id;
}

MRB_API mrb_int
mrb_obj_id(mrb_value obj)
{
  mrb_int tt = mrb_type(obj);

#define MakeID2(p,t) (mrb_int)(((intptr_t)(p))^(t))
#define MakeID(p)    MakeID2(p,tt)

  switch (tt) {
  case MRB_TT_FREE:
  case MRB_TT_UNDEF:
    return MakeID(0); /* not define */
  case MRB_TT_FALSE:
    if (mrb_nil_p(obj))
      return MakeID(1);
    return MakeID(0);
  case MRB_TT_TRUE:
    return MakeID(1);
  case MRB_TT_SYMBOL:
    return MakeID(mrb_symbol(obj));
  case MRB_TT_FIXNUM:
#ifdef MRB_WITHOUT_FLOAT
    return MakeID(mrb_fixnum_id(mrb_fixnum(obj)));
#else
    return MakeID2(mrb_float_id((mrb_float)mrb_fixnum(obj)), MRB_TT_FLOAT);
  case MRB_TT_FLOAT:
    return MakeID(mrb_float_id(mrb_float(obj)));
#endif
  case MRB_TT_STRING:
  case MRB_TT_OBJECT:
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_ICLASS:
  case MRB_TT_SCLASS:
  case MRB_TT_PROC:
  case MRB_TT_ARRAY:
  case MRB_TT_HASH:
  case MRB_TT_RANGE:
  case MRB_TT_EXCEPTION:
  case MRB_TT_DATA:
  case MRB_TT_ISTRUCT:
  default:
    return MakeID(mrb_ptr(obj));
  }
}

#if defined(MRB_NAN_BOXING) && defined(MRB_64BIT)
#define mrb_xxx_boxing_cptr_value mrb_nan_boxing_cptr_value
#endif

#ifdef MRB_WORD_BOXING
#define mrb_xxx_boxing_cptr_value mrb_word_boxing_cptr_value

#ifndef MRB_WITHOUT_FLOAT
MRB_API mrb_value
mrb_word_boxing_float_value(mrb_state *mrb, mrb_float f)
{
  mrb_value v;

  v.value.p = mrb_obj_alloc(mrb, MRB_TT_FLOAT, mrb->float_class);
  v.value.fp->f = f;
  MRB_SET_FROZEN_FLAG(v.value.bp);
  return v;
}

MRB_API mrb_value
mrb_word_boxing_float_pool(mrb_state *mrb, mrb_float f)
{
  struct RFloat *nf = (struct RFloat *)mrb_malloc(mrb, sizeof(struct RFloat));
  nf->tt = MRB_TT_FLOAT;
  nf->c = mrb->float_class;
  nf->f = f;
  MRB_SET_FROZEN_FLAG(nf);
  return mrb_obj_value(nf);
}
#endif  /* MRB_WITHOUT_FLOAT */
#endif  /* MRB_WORD_BOXING */

#if defined(MRB_WORD_BOXING) || (defined(MRB_NAN_BOXING) && defined(MRB_64BIT))
MRB_API mrb_value
mrb_xxx_boxing_cptr_value(mrb_state *mrb, void *p)
{
  mrb_value v;
  struct RCptr *cptr = (struct RCptr*)mrb_obj_alloc(mrb, MRB_TT_CPTR, mrb->object_class);

  SET_OBJ_VALUE(v, cptr);
  cptr->p = p;
  return v;
}
#endif

#if defined _MSC_VER && _MSC_VER < 1900

#ifndef va_copy
static void
mrb_msvc_va_copy(va_list *dest, va_list src)
{
  *dest = src;
}
#define va_copy(dest, src) mrb_msvc_va_copy(&(dest), src)
#endif

MRB_API int
mrb_msvc_vsnprintf(char *s, size_t n, const char *format, va_list arg)
{
  int cnt;
  va_list argcp;
  va_copy(argcp, arg);
  if (n == 0 || (cnt = _vsnprintf_s(s, n, _TRUNCATE, format, argcp)) < 0) {
    cnt = _vscprintf(format, arg);
  }
  va_end(argcp);
  return cnt;
}

MRB_API int
mrb_msvc_snprintf(char *s, size_t n, const char *format, ...)
{
  va_list arg;
  int ret;
  va_start(arg, format);
  ret = mrb_msvc_vsnprintf(s, n, format, arg);
  va_end(arg);
  return ret;
}

#endif  /* defined _MSC_VER && _MSC_VER < 1900 */
#ifndef MRB_WITHOUT_FLOAT
#if defined(MRB_DISABLE_STDIO) || defined(_WIN32) || defined(_WIN64)
/*

Most code in this file originates from musl (src/stdio/vfprintf.c)
which, just like mruby itself, is licensed under the MIT license.

Copyright (c) 2005-2014 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include <limits.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <ctype.h>

#include <mruby.h>
#include <mruby/string.h>

struct fmt_args;

typedef void output_func(struct fmt_args *f, const char *s, size_t l);

struct fmt_args {
  mrb_state *mrb;
  output_func *output;
  void *opaque;
};

struct mrb_cstr {
  char *buf;
  size_t len;
};

#define MAX(a,b) ((a)>(b) ? (a) : (b))
#define MIN(a,b) ((a)<(b) ? (a) : (b))

/* Convenient bit representation for modifier flags, which all fall
 * within 31 codepoints of the space character. */

#define ALT_FORM   (1U<<('#'-' '))
#define ZERO_PAD   (1U<<('0'-' '))
#define LEFT_ADJ   (1U<<('-'-' '))
#define PAD_POS    (1U<<(' '-' '))
#define MARK_POS   (1U<<('+'-' '))

#define FLAGMASK (ALT_FORM|ZERO_PAD|LEFT_ADJ|PAD_POS|MARK_POS)

static output_func strcat_value;
static output_func strcat_cstr;

static void
strcat_value(struct fmt_args *f, const char *s, size_t l)
{
  mrb_value str = *(mrb_value*)f->opaque;
  mrb_str_cat(f->mrb, str, s, l);
}

static void
strcat_cstr(struct fmt_args *f, const char *s, size_t l)
{
  struct mrb_cstr *cstr = (struct mrb_cstr*)f->opaque;

  if (l > cstr->len) {
    mrb_state *mrb = f->mrb;

    mrb_raise(mrb, E_ARGUMENT_ERROR, "string buffer too small");
  }

  memcpy(cstr->buf, s, l);

  cstr->buf += l;
  cstr->len -= l;
}

static void
out(struct fmt_args *f, const char *s, size_t l)
{
  f->output(f, s, l);
}

#define PAD_SIZE 256
static void
pad(struct fmt_args *f, char c, ptrdiff_t w, ptrdiff_t l, uint32_t fl)
{
  char pad[PAD_SIZE];
  if (fl & (LEFT_ADJ | ZERO_PAD) || l >= w) return;
  l = w - l;
  memset(pad, c, l>PAD_SIZE ? PAD_SIZE : l);
  for (; l >= PAD_SIZE; l -= PAD_SIZE)
    out(f, pad, PAD_SIZE);
  out(f, pad, l);
}

static const char xdigits[16] = {
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

static char*
fmt_u(uint32_t x, char *s)
{
  for (; x; x /= 10) *--s = '0' + x % 10;
  return s;
}

/* Do not override this check. The floating point printing code below
 * depends on the float.h constants being right. If they are wrong, it
 * may overflow the stack. */
#if LDBL_MANT_DIG == 53
typedef char compiler_defines_long_double_incorrectly[9-(int)sizeof(long double)];
#endif

static int
fmt_fp(struct fmt_args *f, long double y, ptrdiff_t w, ptrdiff_t p, uint32_t fl, int t)
{
  uint32_t big[(LDBL_MANT_DIG+28)/29 + 1          // mantissa expansion
    + (LDBL_MAX_EXP+LDBL_MANT_DIG+28+8)/9]; // exponent expansion
  uint32_t *a, *d, *r, *z;
  uint32_t i;
  int e2=0, e, j;
  ptrdiff_t l;
  char buf[9+LDBL_MANT_DIG/4], *s;
  const char *prefix="-0X+0X 0X-0x+0x 0x";
  ptrdiff_t pl;
  char ebuf0[3*sizeof(int)], *ebuf=&ebuf0[3*sizeof(int)], *estr;

  pl=1;
  if (signbit(y)) {
    y=-y;
  } else if (fl & MARK_POS) {
    prefix+=3;
  } else if (fl & PAD_POS) {
    prefix+=6;
  } else prefix++, pl=0;

  if (!isfinite(y)) {
    const char *ss = (t&32)?"inf":"INF";
    if (y!=y) ss=(t&32)?"nan":"NAN";
    pad(f, ' ', w, 3+pl, fl&~ZERO_PAD);
    out(f, prefix, pl);
    out(f, ss, 3);
    pad(f, ' ', w, 3+pl, fl^LEFT_ADJ);
    return (int)MAX(w, 3+pl);
  }

  y = frexp((double)y, &e2) * 2;
  if (y) e2--;

  if ((t|32)=='a') {
    long double round = 8.0;
    ptrdiff_t re;

    if (t&32) prefix += 9;
    pl += 2;

    if (p<0 || p>=LDBL_MANT_DIG/4-1) re=0;
    else re=LDBL_MANT_DIG/4-1-p;

    if (re) {
      while (re--) round*=16;
      if (*prefix=='-') {
        y=-y;
        y-=round;
        y+=round;
        y=-y;
      }
      else {
        y+=round;
        y-=round;
      }
    }

    estr=fmt_u(e2<0 ? -e2 : e2, ebuf);
    if (estr==ebuf) *--estr='0';
    *--estr = (e2<0 ? '-' : '+');
    *--estr = t+('p'-'a');

    s=buf;
    do {
      int x=(int)y;
      *s++=xdigits[x]|(t&32);
      y=16*(y-x);
      if (s-buf==1 && (y||p>0||(fl&ALT_FORM))) *s++='.';
    } while (y);

    if (p && s-buf-2 < p)
      l = (p+2) + (ebuf-estr);
    else
      l = (s-buf) + (ebuf-estr);

    pad(f, ' ', w, pl+l, fl);
    out(f, prefix, pl);
    pad(f, '0', w, pl+l, fl^ZERO_PAD);
    out(f, buf, s-buf);
    pad(f, '0', l-(ebuf-estr)-(s-buf), 0, 0);
    out(f, estr, ebuf-estr);
    pad(f, ' ', w, pl+l, fl^LEFT_ADJ);
    return (int)MAX(w, pl+l);
  }
  if (p<0) p=6;

  if (y) y *= 268435456.0, e2-=28;

  if (e2<0) a=r=z=big;
  else a=r=z=big+sizeof(big)/sizeof(*big) - LDBL_MANT_DIG - 1;

  do {
    *z = (uint32_t)y;
    y = 1000000000*(y-*z++);
  } while (y);

  while (e2>0) {
    uint32_t carry=0;
    int sh=MIN(29,e2);
    for (d=z-1; d>=a; d--) {
      uint64_t x = ((uint64_t)*d<<sh)+carry;
      *d = x % 1000000000;
      carry = (uint32_t)(x / 1000000000);
    }
    if (carry) *--a = carry;
    while (z>a && !z[-1]) z--;
    e2-=sh;
  }
  while (e2<0) {
    uint32_t carry=0, *b;
    int sh=MIN(9,-e2), need=1+((int)p+LDBL_MANT_DIG/3+8)/9;
    for (d=a; d<z; d++) {
      uint32_t rm = *d & ((1<<sh)-1);
      *d = (*d>>sh) + carry;
      carry = (1000000000>>sh) * rm;
    }
    if (!*a) a++;
    if (carry) *z++ = carry;
    /* Avoid (slow!) computation past requested precision */
    b = (t|32)=='f' ? r : a;
    if (z-b > need) z = b+need;
    e2+=sh;
  }

  if (a<z) for (i=10, e=9*(int)(r-a); *a>=i; i*=10, e++);
  else e=0;

  /* Perform rounding: j is precision after the radix (possibly neg) */
  j = (int)p - ((t|32)!='f')*e - ((t|32)=='g' && p);
  if (j < 9*(z-r-1)) {
    uint32_t x;
    /* We avoid C's broken division of negative numbers */
    d = r + 1 + ((j+9*LDBL_MAX_EXP)/9 - LDBL_MAX_EXP);
    j += 9*LDBL_MAX_EXP;
    j %= 9;
    for (i=10, j++; j<9; i*=10, j++);
    x = *d % i;
    /* Are there any significant digits past j? */
    if (x || d+1!=z) {
      long double round = 2/LDBL_EPSILON;
      long double small;
      if (*d/i & 1) round += 2;
      if (x<i/2) small=0.5;
      else if (x==i/2 && d+1==z) small=1.0;
      else small=1.5;
      if (pl && *prefix=='-') round*=-1, small*=-1;
      *d -= x;
      /* Decide whether to round by probing round+small */
      if (round+small != round) {
        *d = *d + i;
        while (*d > 999999999) {
          *d--=0;
          if (d<a) *--a=0;
          (*d)++;
        }
        for (i=10, e=9*(int)(r-a); *a>=i; i*=10, e++);
      }
    }
    if (z>d+1) z=d+1;
  }
  for (; z>a && !z[-1]; z--);

  if ((t|32)=='g') {
    if (!p) p++;
    if (p>e && e>=-4) {
      t--;
      p-=e+1;
    }
    else {
      t-=2;
      p--;
    }
    if (!(fl&ALT_FORM)) {
      /* Count trailing zeros in last place */
      if (z>a && z[-1]) for (i=10, j=0; z[-1]%i==0; i*=10, j++);
      else j=9;
      if ((t|32)=='f')
        p = MIN(p,MAX(0,9*(z-r-1)-j));
      else
        p = MIN(p,MAX(0,9*(z-r-1)+e-j));
    }
  }
  l = 1 + p + (p || (fl&ALT_FORM));
  if ((t|32)=='f') {
    if (e>0) l+=e;
  }
  else {
    estr=fmt_u(e<0 ? -e : e, ebuf);
    while(ebuf-estr<2) *--estr='0';
    *--estr = (e<0 ? '-' : '+');
    *--estr = t;
    l += ebuf-estr;
  }

  pad(f, ' ', w, pl+l, fl);
  out(f, prefix, pl);
  pad(f, '0', w, pl+l, fl^ZERO_PAD);

  if ((t|32)=='f') {
    if (a>r) a=r;
    for (d=a; d<=r; d++) {
      char *ss = fmt_u(*d, buf+9);
      if (d!=a) while (ss>buf) *--ss='0';
      else if (ss==buf+9) *--ss='0';
      out(f, ss, buf+9-ss);
    }
    if (p || (fl&ALT_FORM)) out(f, ".", 1);
    for (; d<z && p>0; d++, p-=9) {
      char *ss = fmt_u(*d, buf+9);
      while (ss>buf) *--ss='0';
      out(f, ss, MIN(9,p));
    }
    pad(f, '0', p+9, 9, 0);
  }
  else {
    if (z<=a) z=a+1;
    for (d=a; d<z && p>=0; d++) {
      char *ss = fmt_u(*d, buf+9);
      if (ss==buf+9) *--ss='0';
      if (d!=a) while (ss>buf) *--ss='0';
      else {
        out(f, ss++, 1);
        if (p>0||(fl&ALT_FORM)) out(f, ".", 1);
      }
      out(f, ss, MIN(buf+9-ss, p));
      p -= (int)(buf+9-ss);
    }
    pad(f, '0', p+18, 18, 0);
    out(f, estr, ebuf-estr);
  }

  pad(f, ' ', w, pl+l, fl^LEFT_ADJ);

  return (int)MAX(w, pl+l);
}

static int
fmt_core(struct fmt_args *f, const char *fmt, mrb_float flo)
{
  ptrdiff_t w, p;
  uint32_t fl;

  if (*fmt != '%') {
    return -1;
  }
  ++fmt;

  /* Read modifier flags */
  for (fl=0; (unsigned)*fmt-' '<32 && (FLAGMASK&(1U<<(*fmt-' '))); fmt++)
    fl |= 1U<<(*fmt-' ');

  /* - and 0 flags are mutually exclusive */
  if (fl & LEFT_ADJ) fl &= ~ZERO_PAD;

  for (w = 0; ISDIGIT(*fmt); ++fmt) {
    w = 10 * w + (*fmt - '0');
  }

  if (*fmt == '.') {
    ++fmt;
    for (p = 0; ISDIGIT(*fmt); ++fmt) {
      p = 10 * p + (*fmt - '0');
    }
  }
  else {
    p = -1;
  }

  switch (*fmt) {
  case 'e': case 'f': case 'g': case 'a':
  case 'E': case 'F': case 'G': case 'A':
    return fmt_fp(f, flo, w, p, fl, *fmt);
  default:
    return -1;
  }
}

MRB_API mrb_value
mrb_float_to_str(mrb_state *mrb, mrb_value flo, const char *fmt)
{
  struct fmt_args f;
  mrb_value str = mrb_str_new_capa(mrb, 24);

  f.mrb = mrb;
  f.output = strcat_value;
  f.opaque = (void*)&str;
  if (fmt_core(&f, fmt, mrb_float(flo)) < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid format string");
  }
  return str;
}

MRB_API int
mrb_float_to_cstr(mrb_state *mrb, char *buf, size_t len, const char *fmt, mrb_float fval)
{
  struct fmt_args f;
  struct mrb_cstr cstr;

  cstr.buf = buf;
  cstr.len = len - 1; /* reserve NUL terminator */
  f.mrb = mrb;
  f.output = strcat_cstr;
  f.opaque = (void*)&cstr;
  if (fmt_core(&f, fmt, fval) < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid format string");
  }
  *cstr.buf = '\0';
  return (int)(cstr.buf - buf);
}
#else   /* MRB_DISABLE_STDIO || _WIN32 || _WIN64 */
#include <mruby.h>
#include <stdio.h>

MRB_API mrb_value
mrb_float_to_str(mrb_state *mrb, mrb_value flo, const char *fmt)
{
  char buf[25];

  snprintf(buf, sizeof(buf), fmt, mrb_float(flo));
  return mrb_str_new_cstr(mrb, buf);
}

MRB_API int
mrb_float_to_cstr(mrb_state *mrb, char *buf, size_t len, const char *fmt, mrb_float fval)
{
  return snprintf(buf, len, fmt, fval);
}
#endif  /* MRB_DISABLE_STDIO || _WIN32 || _WIN64 */
#endif
/*
** gc.c - garbage collector for mruby
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/istruct.h>
#include <mruby/hash.h>
#include <mruby/proc.h>
#include <mruby/range.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/gc.h>
#include <mruby/error.h>
#include <mruby/throw.h>

/*
  = Tri-color Incremental Garbage Collection

  mruby's GC is Tri-color Incremental GC with Mark & Sweep.
  Algorithm details are omitted.
  Instead, the implementation part is described below.

  == Object's Color

  Each object can be painted in three colors:

    * White - Unmarked.
    * Gray - Marked, But the child objects are unmarked.
    * Black - Marked, the child objects are also marked.

  == Two White Types

  There're two white color types in a flip-flop fashion: White-A and White-B,
  which respectively represent the Current White color (the newly allocated
  objects in the current GC cycle) and the Sweep Target White color (the
  dead objects to be swept).

  A and B will be switched just at the beginning of the next GC cycle. At
  that time, all the dead objects have been swept, while the newly created
  objects in the current GC cycle which finally remains White are now
  regarded as dead objects. Instead of traversing all the White-A objects and
  painting them as White-B, just switch the meaning of White-A and White-B as
  this will be much cheaper.

  As a result, the objects we sweep in the current GC cycle are always
  left from the previous GC cycle. This allows us to sweep objects
  incrementally, without the disturbance of the newly created objects.

  == Execution Timing

  GC Execution Time and Each step interval are decided by live objects count.
  List of Adjustment API:

    * gc_interval_ratio_set
    * gc_step_ratio_set

  For details, see the comments for each function.

  == Write Barrier

  mruby implementer and C extension library writer must insert a write
  barrier when updating a reference from a field of an object.
  When updating a reference from a field of object A to object B,
  two different types of write barrier are available:

    * mrb_field_write_barrier - target B object for a mark.
    * mrb_write_barrier       - target A object for a mark.

  == Generational Mode

  mruby's GC offers an Generational Mode while re-using the tri-color GC
  infrastructure. It will treat the Black objects as Old objects after each
  sweep phase, instead of painting them White. The key ideas are still the same
  as traditional generational GC:

    * Minor GC - just traverse the Young objects (Gray objects) in the mark
                 phase, then only sweep the newly created objects, and leave
                 the Old objects live.

    * Major GC - same as a full regular GC cycle.

  The difference from "traditional" generational GC is, that the major GC
  in mruby is triggered incrementally in a tri-color manner.


  For details, see the comments for each function.

*/

struct free_obj {
  MRB_OBJECT_HEADER;
  struct RBasic *next;
};

typedef struct {
  union {
    struct free_obj free;
    struct RBasic basic;
    struct RObject object;
    struct RClass klass;
    struct RString string;
    struct RArray array;
    struct RHash hash;
    struct RRange range;
    struct RData data;
    struct RIStruct istruct;
    struct RProc proc;
    struct REnv env;
    struct RFiber fiber;
    struct RException exc;
    struct RBreak brk;
#ifdef MRB_WORD_BOXING
#ifndef MRB_WITHOUT_FLOAT
    struct RFloat floatv;
#endif
    struct RCptr cptr;
#endif
  } as;
} RVALUE;

#ifdef GC_PROFILE
#include <stdio.h>
#include <sys/time.h>

static double program_invoke_time = 0;
static double gc_time = 0;
static double gc_total_time = 0;

static double
gettimeofday_time(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

#define GC_INVOKE_TIME_REPORT(with) do {\
  fprintf(stderr, "%s\n", with);\
  fprintf(stderr, "gc_invoke: %19.3f\n", gettimeofday_time() - program_invoke_time);\
  fprintf(stderr, "is_generational: %d\n", is_generational(gc));\
  fprintf(stderr, "is_major_gc: %d\n", is_major_gc(gc));\
} while(0)

#define GC_TIME_START do {\
  gc_time = gettimeofday_time();\
} while(0)

#define GC_TIME_STOP_AND_REPORT do {\
  gc_time = gettimeofday_time() - gc_time;\
  gc_total_time += gc_time;\
  fprintf(stderr, "gc_state: %d\n", gc->state);\
  fprintf(stderr, "live: %zu\n", gc->live);\
  fprintf(stderr, "majorgc_old_threshold: %zu\n", gc->majorgc_old_threshold);\
  fprintf(stderr, "gc_threshold: %zu\n", gc->threshold);\
  fprintf(stderr, "gc_time: %30.20f\n", gc_time);\
  fprintf(stderr, "gc_total_time: %30.20f\n\n", gc_total_time);\
} while(0)
#else
#define GC_INVOKE_TIME_REPORT(s)
#define GC_TIME_START
#define GC_TIME_STOP_AND_REPORT
#endif

#ifdef GC_DEBUG
#define DEBUG(x) (x)
#else
#define DEBUG(x)
#endif

#ifndef MRB_HEAP_PAGE_SIZE
#define MRB_HEAP_PAGE_SIZE 1024
#endif

#define GC_STEP_SIZE 1024

/* white: 001 or 010, black: 100, gray: 000 */
#define GC_GRAY 0
#define GC_WHITE_A 1
#define GC_WHITE_B (1 << 1)
#define GC_BLACK (1 << 2)
#define GC_WHITES (GC_WHITE_A | GC_WHITE_B)
#define GC_COLOR_MASK 7

#define paint_gray(o) ((o)->color = GC_GRAY)
#define paint_black(o) ((o)->color = GC_BLACK)
#define paint_white(o) ((o)->color = GC_WHITES)
#define paint_partial_white(s, o) ((o)->color = (s)->current_white_part)
#define is_gray(o) ((o)->color == GC_GRAY)
#define is_white(o) ((o)->color & GC_WHITES)
#define is_black(o) ((o)->color & GC_BLACK)
#define flip_white_part(s) ((s)->current_white_part = other_white_part(s))
#define other_white_part(s) ((s)->current_white_part ^ GC_WHITES)
#define is_dead(s, o) (((o)->color & other_white_part(s) & GC_WHITES) || (o)->tt == MRB_TT_FREE)

#define objects(p) ((RVALUE *)p->objects)

mrb_noreturn void mrb_raise_nomemory(mrb_state *mrb);

MRB_API void*
mrb_realloc_simple(mrb_state *mrb, void *p,  size_t len)
{
  void *p2;

  p2 = (mrb->allocf)(mrb, p, len, mrb->allocf_ud);
  if (!p2 && len > 0 && mrb->gc.heaps) {
    mrb_full_gc(mrb);
    p2 = (mrb->allocf)(mrb, p, len, mrb->allocf_ud);
  }

  return p2;
}

MRB_API void*
mrb_realloc(mrb_state *mrb, void *p, size_t len)
{
  void *p2;

  p2 = mrb_realloc_simple(mrb, p, len);
  if (len == 0) return p2;
  if (p2 == NULL) {
    if (mrb->gc.out_of_memory) {
      mrb_raise_nomemory(mrb);
      /* mrb_panic(mrb); */
    }
    else {
      mrb->gc.out_of_memory = TRUE;
      mrb_raise_nomemory(mrb);
    }
  }
  else {
    mrb->gc.out_of_memory = FALSE;
  }

  return p2;
}

MRB_API void*
mrb_malloc(mrb_state *mrb, size_t len)
{
  return mrb_realloc(mrb, 0, len);
}

MRB_API void*
mrb_malloc_simple(mrb_state *mrb, size_t len)
{
  return mrb_realloc_simple(mrb, 0, len);
}

MRB_API void*
mrb_calloc(mrb_state *mrb, size_t nelem, size_t len)
{
  void *p;

  if (nelem > 0 && len > 0 &&
      nelem <= SIZE_MAX / len) {
    size_t size;
    size = nelem * len;
    p = mrb_malloc(mrb, size);

    memset(p, 0, size);
  }
  else {
    p = NULL;
  }

  return p;
}

MRB_API void
mrb_free(mrb_state *mrb, void *p)
{
  (mrb->allocf)(mrb, p, 0, mrb->allocf_ud);
}

MRB_API void*
mrb_alloca(mrb_state *mrb, size_t size)
{
  struct RString *s;
  s = (struct RString*)mrb_obj_alloc(mrb, MRB_TT_STRING, mrb->string_class);
  return s->as.heap.ptr = (char*)mrb_malloc(mrb, size);
}

static mrb_bool
heap_p(mrb_gc *gc, struct RBasic *object)
{
  mrb_heap_page* page;

  page = gc->heaps;
  while (page) {
    RVALUE *p;

    p = objects(page);
    if (&p[0].as.basic <= object && object <= &p[MRB_HEAP_PAGE_SIZE].as.basic) {
      return TRUE;
    }
    page = page->next;
  }
  return FALSE;
}

MRB_API mrb_bool
mrb_object_dead_p(mrb_state *mrb, struct RBasic *object) {
  mrb_gc *gc = &mrb->gc;
  if (!heap_p(gc, object)) return TRUE;
  return is_dead(gc, object);
}

static void
link_heap_page(mrb_gc *gc, mrb_heap_page *page)
{
  page->next = gc->heaps;
  if (gc->heaps)
    gc->heaps->prev = page;
  gc->heaps = page;
}

static void
unlink_heap_page(mrb_gc *gc, mrb_heap_page *page)
{
  if (page->prev)
    page->prev->next = page->next;
  if (page->next)
    page->next->prev = page->prev;
  if (gc->heaps == page)
    gc->heaps = page->next;
  page->prev = NULL;
  page->next = NULL;
}

static void
link_free_heap_page(mrb_gc *gc, mrb_heap_page *page)
{
  page->free_next = gc->free_heaps;
  if (gc->free_heaps) {
    gc->free_heaps->free_prev = page;
  }
  gc->free_heaps = page;
}

static void
unlink_free_heap_page(mrb_gc *gc, mrb_heap_page *page)
{
  if (page->free_prev)
    page->free_prev->free_next = page->free_next;
  if (page->free_next)
    page->free_next->free_prev = page->free_prev;
  if (gc->free_heaps == page)
    gc->free_heaps = page->free_next;
  page->free_prev = NULL;
  page->free_next = NULL;
}

static void
add_heap(mrb_state *mrb, mrb_gc *gc)
{
  mrb_heap_page *page = (mrb_heap_page *)mrb_calloc(mrb, 1, sizeof(mrb_heap_page) + MRB_HEAP_PAGE_SIZE * sizeof(RVALUE));
  RVALUE *p, *e;
  struct RBasic *prev = NULL;

  for (p = objects(page), e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
    p->as.free.tt = MRB_TT_FREE;
    p->as.free.next = prev;
    prev = &p->as.basic;
  }
  page->freelist = prev;

  link_heap_page(gc, page);
  link_free_heap_page(gc, page);
}

#define DEFAULT_GC_INTERVAL_RATIO 200
#define DEFAULT_GC_STEP_RATIO 200
#define MAJOR_GC_INC_RATIO 120
#define MAJOR_GC_TOOMANY 10000
#define is_generational(gc) ((gc)->generational)
#define is_major_gc(gc) (is_generational(gc) && (gc)->full)
#define is_minor_gc(gc) (is_generational(gc) && !(gc)->full)

void
mrb_gc_init(mrb_state *mrb, mrb_gc *gc)
{
#ifndef MRB_GC_FIXED_ARENA
  gc->arena = (struct RBasic**)mrb_malloc(mrb, sizeof(struct RBasic*)*MRB_GC_ARENA_SIZE);
  gc->arena_capa = MRB_GC_ARENA_SIZE;
#endif

  gc->current_white_part = GC_WHITE_A;
  gc->heaps = NULL;
  gc->free_heaps = NULL;
  add_heap(mrb, gc);
  gc->interval_ratio = DEFAULT_GC_INTERVAL_RATIO;
  gc->step_ratio = DEFAULT_GC_STEP_RATIO;
#ifndef MRB_GC_TURN_OFF_GENERATIONAL
  gc->generational = TRUE;
  gc->full = TRUE;
#endif

#ifdef GC_PROFILE
  program_invoke_time = gettimeofday_time();
#endif
}

static void obj_free(mrb_state *mrb, struct RBasic *obj, int end);

static void
free_heap(mrb_state *mrb, mrb_gc *gc)
{
  mrb_heap_page *page = gc->heaps;
  mrb_heap_page *tmp;
  RVALUE *p, *e;

  while (page) {
    tmp = page;
    page = page->next;
    for (p = objects(tmp), e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
      if (p->as.free.tt != MRB_TT_FREE)
        obj_free(mrb, &p->as.basic, TRUE);
    }
    mrb_free(mrb, tmp);
  }
}

void
mrb_gc_destroy(mrb_state *mrb, mrb_gc *gc)
{
  free_heap(mrb, gc);
#ifndef MRB_GC_FIXED_ARENA
  mrb_free(mrb, gc->arena);
#endif
}

static void
gc_protect(mrb_state *mrb, mrb_gc *gc, struct RBasic *p)
{
#ifdef MRB_GC_FIXED_ARENA
  if (gc->arena_idx >= MRB_GC_ARENA_SIZE) {
    /* arena overflow error */
    gc->arena_idx = MRB_GC_ARENA_SIZE - 4; /* force room in arena */
    mrb_exc_raise(mrb, mrb_obj_value(mrb->arena_err));
  }
#else
  if (gc->arena_idx >= gc->arena_capa) {
    /* extend arena */
    gc->arena_capa = (int)(gc->arena_capa * 3 / 2);
    gc->arena = (struct RBasic**)mrb_realloc(mrb, gc->arena, sizeof(struct RBasic*)*gc->arena_capa);
  }
#endif
  gc->arena[gc->arena_idx++] = p;
}

/* mrb_gc_protect() leaves the object in the arena */
MRB_API void
mrb_gc_protect(mrb_state *mrb, mrb_value obj)
{
  if (mrb_immediate_p(obj)) return;
  gc_protect(mrb, &mrb->gc, mrb_basic_ptr(obj));
}

#define GC_ROOT_NAME "_gc_root_"

/* mrb_gc_register() keeps the object from GC.

   Register your object when it's exported to C world,
   without reference from Ruby world, e.g. callback
   arguments.  Don't forget to remove the object using
   mrb_gc_unregister, otherwise your object will leak.
*/

MRB_API void
mrb_gc_register(mrb_state *mrb, mrb_value obj)
{
  mrb_sym root;
  mrb_value table;

  if (mrb_immediate_p(obj)) return;
  root = mrb_intern_lit(mrb, GC_ROOT_NAME);
  table = mrb_gv_get(mrb, root);
  if (mrb_nil_p(table) || !mrb_array_p(table)) {
    table = mrb_ary_new(mrb);
    mrb_gv_set(mrb, root, table);
  }
  mrb_ary_push(mrb, table, obj);
}

/* mrb_gc_unregister() removes the object from GC root. */
MRB_API void
mrb_gc_unregister(mrb_state *mrb, mrb_value obj)
{
  mrb_sym root;
  mrb_value table;
  struct RArray *a;
  mrb_int i;

  if (mrb_immediate_p(obj)) return;
  root = mrb_intern_lit(mrb, GC_ROOT_NAME);
  table = mrb_gv_get(mrb, root);
  if (mrb_nil_p(table)) return;
  if (!mrb_array_p(table)) {
    mrb_gv_set(mrb, root, mrb_nil_value());
    return;
  }
  a = mrb_ary_ptr(table);
  mrb_ary_modify(mrb, a);
  for (i = 0; i < ARY_LEN(a); i++) {
    if (mrb_ptr(ARY_PTR(a)[i]) == mrb_ptr(obj)) {
      mrb_int len = ARY_LEN(a)-1;
      mrb_value *ptr = ARY_PTR(a);

      ARY_SET_LEN(a, len);
      memmove(&ptr[i], &ptr[i + 1], (len - i) * sizeof(mrb_value));
      break;
    }
  }
}

MRB_API struct RBasic*
mrb_obj_alloc(mrb_state *mrb, enum mrb_vtype ttype, struct RClass *cls)
{
  struct RBasic *p;
  static const RVALUE RVALUE_zero = { { { NULL, NULL, MRB_TT_FALSE } } };
  mrb_gc *gc = &mrb->gc;

  if (cls) {
    enum mrb_vtype tt;

    switch (cls->tt) {
    case MRB_TT_CLASS:
    case MRB_TT_SCLASS:
    case MRB_TT_MODULE:
    case MRB_TT_ENV:
      break;
    default:
      mrb_raise(mrb, E_TYPE_ERROR, "allocation failure");
    }
    tt = MRB_INSTANCE_TT(cls);
    if (tt != MRB_TT_FALSE &&
        ttype != MRB_TT_SCLASS &&
        ttype != MRB_TT_ICLASS &&
        ttype != MRB_TT_ENV &&
        ttype != tt) {
      mrb_raisef(mrb, E_TYPE_ERROR, "allocation failure of %C", cls);
    }
  }

#ifdef MRB_GC_STRESS
  mrb_full_gc(mrb);
#endif
  if (gc->threshold < gc->live) {
    mrb_incremental_gc(mrb);
  }
  if (gc->free_heaps == NULL) {
    add_heap(mrb, gc);
  }

  p = gc->free_heaps->freelist;
  gc->free_heaps->freelist = ((struct free_obj*)p)->next;
  if (gc->free_heaps->freelist == NULL) {
    unlink_free_heap_page(gc, gc->free_heaps);
  }

  gc->live++;
  gc_protect(mrb, gc, p);
  *(RVALUE *)p = RVALUE_zero;
  p->tt = ttype;
  p->c = cls;
  paint_partial_white(gc, p);
  return p;
}

static inline void
add_gray_list(mrb_state *mrb, mrb_gc *gc, struct RBasic *obj)
{
#ifdef MRB_GC_STRESS
  if (obj->tt > MRB_TT_MAXDEFINE) {
    abort();
  }
#endif
  paint_gray(obj);
  obj->gcnext = gc->gray_list;
  gc->gray_list = obj;
}

static int
gc_ci_nregs(mrb_callinfo *ci)
{
  struct RProc *p = ci->proc;
  int n = 0;

  if (!p) {
    if (ci->argc < 0) return 3;
    return ci->argc+2;
  }
  if (!MRB_PROC_CFUNC_P(p) && p->body.irep) {
    n = p->body.irep->nregs;
  }
  if (ci->argc < 0) {
    if (n < 3) n = 3; /* self + args + blk */
  }
  if (ci->argc > n) {
    n = ci->argc + 2; /* self + blk */
  }
  return n;
}

static void
mark_context_stack(mrb_state *mrb, struct mrb_context *c)
{
  size_t i;
  size_t e;
  mrb_value nil;

  if (c->stack == NULL) return;
  e = c->stack - c->stbase;
  if (c->ci) {
    e += gc_ci_nregs(c->ci);
  }
  if (c->stbase + e > c->stend) e = c->stend - c->stbase;
  for (i=0; i<e; i++) {
    mrb_value v = c->stbase[i];

    if (!mrb_immediate_p(v)) {
      mrb_gc_mark(mrb, mrb_basic_ptr(v));
    }
  }
  e = c->stend - c->stbase;
  nil = mrb_nil_value();
  for (; i<e; i++) {
    c->stbase[i] = nil;
  }
}

static void
mark_context(mrb_state *mrb, struct mrb_context *c)
{
  int i;
  mrb_callinfo *ci;

 start:
  if (c->status == MRB_FIBER_TERMINATED) return;

  /* mark VM stack */
  mark_context_stack(mrb, c);

  /* mark call stack */
  if (c->cibase) {
    for (ci = c->cibase; ci <= c->ci; ci++) {
      mrb_gc_mark(mrb, (struct RBasic*)ci->env);
      mrb_gc_mark(mrb, (struct RBasic*)ci->proc);
      mrb_gc_mark(mrb, (struct RBasic*)ci->target_class);
    }
  }
  /* mark ensure stack */
  for (i=0; i<c->eidx; i++) {
    mrb_gc_mark(mrb, (struct RBasic*)c->ensure[i]);
  }
  /* mark fibers */
  mrb_gc_mark(mrb, (struct RBasic*)c->fib);
  if (c->prev) {
    c = c->prev;
    goto start;
  }
}

static void
gc_mark_children(mrb_state *mrb, mrb_gc *gc, struct RBasic *obj)
{
  mrb_assert(is_gray(obj));
  paint_black(obj);
  gc->gray_list = obj->gcnext;
  mrb_gc_mark(mrb, (struct RBasic*)obj->c);
  switch (obj->tt) {
  case MRB_TT_ICLASS:
    {
      struct RClass *c = (struct RClass*)obj;
      if (MRB_FLAG_TEST(c, MRB_FL_CLASS_IS_ORIGIN))
        mrb_gc_mark_mt(mrb, c);
      mrb_gc_mark(mrb, (struct RBasic*)((struct RClass*)obj)->super);
    }
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    {
      struct RClass *c = (struct RClass*)obj;

      mrb_gc_mark_mt(mrb, c);
      mrb_gc_mark(mrb, (struct RBasic*)c->super);
    }
    /* fall through */

  case MRB_TT_OBJECT:
  case MRB_TT_DATA:
  case MRB_TT_EXCEPTION:
    mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_PROC:
    {
      struct RProc *p = (struct RProc*)obj;

      mrb_gc_mark(mrb, (struct RBasic*)p->upper);
      mrb_gc_mark(mrb, (struct RBasic*)p->e.env);
    }
    break;

  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;
      mrb_int i, len;

      if (MRB_ENV_STACK_SHARED_P(e) && e->cxt && e->cxt->fib) {
        mrb_gc_mark(mrb, (struct RBasic*)e->cxt->fib);
      }
      len = MRB_ENV_STACK_LEN(e);
      for (i=0; i<len; i++) {
        mrb_gc_mark_value(mrb, e->stack[i]);
      }
    }
    break;

  case MRB_TT_FIBER:
    {
      struct mrb_context *c = ((struct RFiber*)obj)->cxt;

      if (c) mark_context(mrb, c);
    }
    break;

  case MRB_TT_ARRAY:
    {
      struct RArray *a = (struct RArray*)obj;
      size_t i, e;

      for (i=0,e=ARY_LEN(a); i<e; i++) {
        mrb_gc_mark_value(mrb, ARY_PTR(a)[i]);
      }
    }
    break;

  case MRB_TT_HASH:
    mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    mrb_gc_mark_hash(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    if (RSTR_FSHARED_P(obj)) {
      struct RString *s = (struct RString*)obj;
      mrb_gc_mark(mrb, (struct RBasic*)s->as.heap.aux.fshared);
    }
    break;

  case MRB_TT_RANGE:
    mrb_gc_mark_range(mrb, (struct RRange*)obj);
    break;

  default:
    break;
  }
}

MRB_API void
mrb_gc_mark(mrb_state *mrb, struct RBasic *obj)
{
  if (obj == 0) return;
  if (!is_white(obj)) return;
  mrb_assert((obj)->tt != MRB_TT_FREE);
  add_gray_list(mrb, &mrb->gc, obj);
}

static void
obj_free(mrb_state *mrb, struct RBasic *obj, int end)
{
  DEBUG(fprintf(stderr, "obj_free(%p,tt=%d)\n",obj,obj->tt));
  switch (obj->tt) {
    /* immediate - no mark */
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
  case MRB_TT_SYMBOL:
    /* cannot happen */
    return;

#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
#ifdef MRB_WORD_BOXING
    break;
#else
    return;
#endif
#endif

  case MRB_TT_OBJECT:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_EXCEPTION:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    mrb_gc_free_mt(mrb, (struct RClass*)obj);
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    mrb_mc_clear_by_class(mrb, (struct RClass*)obj);
    break;
  case MRB_TT_ICLASS:
    if (MRB_FLAG_TEST(obj, MRB_FL_CLASS_IS_ORIGIN))
      mrb_gc_free_mt(mrb, (struct RClass*)obj);
    mrb_mc_clear_by_class(mrb, (struct RClass*)obj);
    break;
  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;

      if (MRB_ENV_STACK_SHARED_P(e)) {
        /* cannot be freed */
        e->stack = NULL;
        break;
      }
      mrb_free(mrb, e->stack);
      e->stack = NULL;
    }
    break;

  case MRB_TT_FIBER:
    {
      struct mrb_context *c = ((struct RFiber*)obj)->cxt;

      if (c && c != mrb->root_c) {
        if (!end && c->status != MRB_FIBER_TERMINATED) {
          mrb_callinfo *ci = c->ci;
          mrb_callinfo *ce = c->cibase;

          while (ce <= ci) {
            struct REnv *e = ci->env;
            if (e && !mrb_object_dead_p(mrb, (struct RBasic*)e) &&
                e->tt == MRB_TT_ENV && MRB_ENV_STACK_SHARED_P(e)) {
              mrb_env_unshare(mrb, e);
            }
            ci--;
          }
        }
        mrb_free_context(mrb, c);
      }
    }
    break;

  case MRB_TT_ARRAY:
    if (ARY_SHARED_P(obj))
      mrb_ary_decref(mrb, ((struct RArray*)obj)->as.heap.aux.shared);
    else if (!ARY_EMBED_P(obj))
      mrb_free(mrb, ((struct RArray*)obj)->as.heap.ptr);
    break;

  case MRB_TT_HASH:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    mrb_gc_free_hash(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    mrb_gc_free_str(mrb, (struct RString*)obj);
    break;

  case MRB_TT_PROC:
    {
      struct RProc *p = (struct RProc*)obj;

      if (!MRB_PROC_CFUNC_P(p) && p->body.irep) {
        mrb_irep *irep = p->body.irep;
        if (end) {
          mrb_irep_cutref(mrb, irep);
        }
        mrb_irep_decref(mrb, irep);
      }
    }
    break;

  case MRB_TT_RANGE:
    mrb_gc_free_range(mrb, ((struct RRange*)obj));
    break;

  case MRB_TT_DATA:
    {
      struct RData *d = (struct RData*)obj;
      if (d->type && d->type->dfree) {
        d->type->dfree(mrb, d->data);
      }
      mrb_gc_free_iv(mrb, (struct RObject*)obj);
    }
    break;

  default:
    break;
  }
  obj->tt = MRB_TT_FREE;
}

static void
root_scan_phase(mrb_state *mrb, mrb_gc *gc)
{
  int i, e;

  if (!is_minor_gc(gc)) {
    gc->gray_list = NULL;
    gc->atomic_gray_list = NULL;
  }

  mrb_gc_mark_gv(mrb);
  /* mark arena */
  for (i=0,e=gc->arena_idx; i<e; i++) {
    mrb_gc_mark(mrb, gc->arena[i]);
  }
  /* mark class hierarchy */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->object_class);

  /* mark built-in classes */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->class_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->module_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->proc_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->string_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->array_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->hash_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->range_class);

#ifndef MRB_WITHOUT_FLOAT
  mrb_gc_mark(mrb, (struct RBasic*)mrb->float_class);
#endif
  mrb_gc_mark(mrb, (struct RBasic*)mrb->fixnum_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->true_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->false_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->nil_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->symbol_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->kernel_module);

  mrb_gc_mark(mrb, (struct RBasic*)mrb->eException_class);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->eStandardError_class);

  /* mark top_self */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->top_self);
  /* mark exception */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->exc);
  /* mark pre-allocated exception */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->nomem_err);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->stack_err);
#ifdef MRB_GC_FIXED_ARENA
  mrb_gc_mark(mrb, (struct RBasic*)mrb->arena_err);
#endif

  mark_context(mrb, mrb->c);
  if (mrb->root_c != mrb->c) {
    mark_context(mrb, mrb->root_c);
  }
}

/* rough estimation of number of GC marks (non recursive) */
static size_t
gc_gray_counts(mrb_state *mrb, mrb_gc *gc, struct RBasic *obj)
{
  size_t children = 0;

  switch (obj->tt) {
  case MRB_TT_ICLASS:
    children++;
    break;

  case MRB_TT_CLASS:
  case MRB_TT_SCLASS:
  case MRB_TT_MODULE:
    {
      struct RClass *c = (struct RClass*)obj;

      children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
      children += mrb_gc_mark_mt_size(mrb, c);
      children++;
    }
    break;

  case MRB_TT_OBJECT:
  case MRB_TT_DATA:
  case MRB_TT_EXCEPTION:
    children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_ENV:
    children += MRB_ENV_STACK_LEN(obj);
    break;

  case MRB_TT_FIBER:
    {
      struct mrb_context *c = ((struct RFiber*)obj)->cxt;
      size_t i;
      mrb_callinfo *ci;

      if (!c || c->status == MRB_FIBER_TERMINATED) break;

      /* mark stack */
      i = c->stack - c->stbase;

      if (c->ci) {
        i += gc_ci_nregs(c->ci);
      }
      if (c->stbase + i > c->stend) i = c->stend - c->stbase;
      children += i;

      /* mark ensure stack */
      children += c->eidx;

      /* mark closure */
      if (c->cibase) {
        for (i=0, ci = c->cibase; ci <= c->ci; i++, ci++)
          ;
      }
      children += i;
    }
    break;

  case MRB_TT_ARRAY:
    {
      struct RArray *a = (struct RArray*)obj;
      children += ARY_LEN(a);
    }
    break;

  case MRB_TT_HASH:
    children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
    children += mrb_gc_mark_hash_size(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_PROC:
  case MRB_TT_RANGE:
    children+=2;
    break;

  default:
    break;
  }
  return children;
}


static void
gc_mark_gray_list(mrb_state *mrb, mrb_gc *gc) {
  while (gc->gray_list) {
    if (is_gray(gc->gray_list))
      gc_mark_children(mrb, gc, gc->gray_list);
    else
      gc->gray_list = gc->gray_list->gcnext;
  }
}


static size_t
incremental_marking_phase(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  size_t tried_marks = 0;

  while (gc->gray_list && tried_marks < limit) {
    struct RBasic *obj = gc->gray_list;
    gc_mark_children(mrb, gc, obj);
    tried_marks += gc_gray_counts(mrb, gc, obj);
  }

  return tried_marks;
}

static void
final_marking_phase(mrb_state *mrb, mrb_gc *gc)
{
  int i, e;

  /* mark arena */
  for (i=0,e=gc->arena_idx; i<e; i++) {
    mrb_gc_mark(mrb, gc->arena[i]);
  }
  mrb_gc_mark_gv(mrb);
  mark_context(mrb, mrb->c);
  mark_context(mrb, mrb->root_c);
  mrb_gc_mark(mrb, (struct RBasic*)mrb->exc);
  gc_mark_gray_list(mrb, gc);
  mrb_assert(gc->gray_list == NULL);
  gc->gray_list = gc->atomic_gray_list;
  gc->atomic_gray_list = NULL;
  gc_mark_gray_list(mrb, gc);
  mrb_assert(gc->gray_list == NULL);
}

static void
prepare_incremental_sweep(mrb_state *mrb, mrb_gc *gc)
{
  gc->state = MRB_GC_STATE_SWEEP;
  gc->sweeps = gc->heaps;
  gc->live_after_mark = gc->live;
}

static size_t
incremental_sweep_phase(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  mrb_heap_page *page = gc->sweeps;
  size_t tried_sweep = 0;

  while (page && (tried_sweep < limit)) {
    RVALUE *p = objects(page);
    RVALUE *e = p + MRB_HEAP_PAGE_SIZE;
    size_t freed = 0;
    mrb_bool dead_slot = TRUE;
    mrb_bool full = (page->freelist == NULL);

    if (is_minor_gc(gc) && page->old) {
      /* skip a slot which doesn't contain any young object */
      p = e;
      dead_slot = FALSE;
    }
    while (p<e) {
      if (is_dead(gc, &p->as.basic)) {
        if (p->as.basic.tt != MRB_TT_FREE) {
          obj_free(mrb, &p->as.basic, FALSE);
          if (p->as.basic.tt == MRB_TT_FREE) {
            p->as.free.next = page->freelist;
            page->freelist = (struct RBasic*)p;
            freed++;
          }
          else {
            dead_slot = FALSE;
          }
        }
      }
      else {
        if (!is_generational(gc))
          paint_partial_white(gc, &p->as.basic); /* next gc target */
        dead_slot = FALSE;
      }
      p++;
    }

    /* free dead slot */
    if (dead_slot && freed < MRB_HEAP_PAGE_SIZE) {
      mrb_heap_page *next = page->next;

      unlink_heap_page(gc, page);
      unlink_free_heap_page(gc, page);
      mrb_free(mrb, page);
      page = next;
    }
    else {
      if (full && freed > 0) {
        link_free_heap_page(gc, page);
      }
      if (page->freelist == NULL && is_minor_gc(gc))
        page->old = TRUE;
      else
        page->old = FALSE;
      page = page->next;
    }
    tried_sweep += MRB_HEAP_PAGE_SIZE;
    gc->live -= freed;
    gc->live_after_mark -= freed;
  }
  gc->sweeps = page;
  return tried_sweep;
}

static size_t
incremental_gc(mrb_state *mrb, mrb_gc *gc, size_t limit)
{
  switch (gc->state) {
  case MRB_GC_STATE_ROOT:
    root_scan_phase(mrb, gc);
    gc->state = MRB_GC_STATE_MARK;
    flip_white_part(gc);
    return 0;
  case MRB_GC_STATE_MARK:
    if (gc->gray_list) {
      return incremental_marking_phase(mrb, gc, limit);
    }
    else {
      final_marking_phase(mrb, gc);
      prepare_incremental_sweep(mrb, gc);
      return 0;
    }
  case MRB_GC_STATE_SWEEP: {
     size_t tried_sweep = 0;
     tried_sweep = incremental_sweep_phase(mrb, gc, limit);
     if (tried_sweep == 0)
       gc->state = MRB_GC_STATE_ROOT;
     return tried_sweep;
  }
  default:
    /* unknown state */
    mrb_assert(0);
    return 0;
  }
}

static void
incremental_gc_until(mrb_state *mrb, mrb_gc *gc, mrb_gc_state to_state)
{
  do {
    incremental_gc(mrb, gc, SIZE_MAX);
  } while (gc->state != to_state);
}

static void
incremental_gc_step(mrb_state *mrb, mrb_gc *gc)
{
  size_t limit = 0, result = 0;
  limit = (GC_STEP_SIZE/100) * gc->step_ratio;
  while (result < limit) {
    result += incremental_gc(mrb, gc, limit);
    if (gc->state == MRB_GC_STATE_ROOT)
      break;
  }

  gc->threshold = gc->live + GC_STEP_SIZE;
}

static void
clear_all_old(mrb_state *mrb, mrb_gc *gc)
{
  mrb_bool origin_mode = gc->generational;

  mrb_assert(is_generational(gc));
  if (is_major_gc(gc)) {
    /* finish the half baked GC */
    incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
  }

  /* Sweep the dead objects, then reset all the live objects
   * (including all the old objects, of course) to white. */
  gc->generational = FALSE;
  prepare_incremental_sweep(mrb, gc);
  incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
  gc->generational = origin_mode;

  /* The gray objects have already been painted as white */
  gc->atomic_gray_list = gc->gray_list = NULL;
}

MRB_API void
mrb_incremental_gc(mrb_state *mrb)
{
  mrb_gc *gc = &mrb->gc;

  if (gc->disabled || gc->iterating) return;

  GC_INVOKE_TIME_REPORT("mrb_incremental_gc()");
  GC_TIME_START;

  if (is_minor_gc(gc)) {
    incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
  }
  else {
    incremental_gc_step(mrb, gc);
  }

  if (gc->state == MRB_GC_STATE_ROOT) {
    mrb_assert(gc->live >= gc->live_after_mark);
    gc->threshold = (gc->live_after_mark/100) * gc->interval_ratio;
    if (gc->threshold < GC_STEP_SIZE) {
      gc->threshold = GC_STEP_SIZE;
    }

    if (is_major_gc(gc)) {
      size_t threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;

      gc->full = FALSE;
      if (threshold < MAJOR_GC_TOOMANY) {
        gc->majorgc_old_threshold = threshold;
      }
      else {
        /* too many objects allocated during incremental GC, */
        /* instead of increasing threshold, invoke full GC. */
        mrb_full_gc(mrb);
      }
    }
    else if (is_minor_gc(gc)) {
      if (gc->live > gc->majorgc_old_threshold) {
        clear_all_old(mrb, gc);
        gc->full = TRUE;
      }
    }
  }

  GC_TIME_STOP_AND_REPORT;
}

/* Perform a full gc cycle */
MRB_API void
mrb_full_gc(mrb_state *mrb)
{
  mrb_gc *gc = &mrb->gc;

  if (!mrb->c) return;
  if (gc->disabled || gc->iterating) return;

  GC_INVOKE_TIME_REPORT("mrb_full_gc()");
  GC_TIME_START;

  if (is_generational(gc)) {
    /* clear all the old objects back to young */
    clear_all_old(mrb, gc);
    gc->full = TRUE;
  }
  else if (gc->state != MRB_GC_STATE_ROOT) {
    /* finish half baked GC cycle */
    incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
  }

  incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
  gc->threshold = (gc->live_after_mark/100) * gc->interval_ratio;

  if (is_generational(gc)) {
    gc->majorgc_old_threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;
    gc->full = FALSE;
  }

  GC_TIME_STOP_AND_REPORT;
}

MRB_API void
mrb_garbage_collect(mrb_state *mrb)
{
  mrb_full_gc(mrb);
}

/*
 * Field write barrier
 *   Paint obj(Black) -> value(White) to obj(Black) -> value(Gray).
 */

MRB_API void
mrb_field_write_barrier(mrb_state *mrb, struct RBasic *obj, struct RBasic *value)
{
  mrb_gc *gc = &mrb->gc;

  if (!is_black(obj)) return;
  if (!is_white(value)) return;

  mrb_assert(gc->state == MRB_GC_STATE_MARK || (!is_dead(gc, value) && !is_dead(gc, obj)));
  mrb_assert(is_generational(gc) || gc->state != MRB_GC_STATE_ROOT);

  if (is_generational(gc) || gc->state == MRB_GC_STATE_MARK) {
    add_gray_list(mrb, gc, value);
  }
  else {
    mrb_assert(gc->state == MRB_GC_STATE_SWEEP);
    paint_partial_white(gc, obj); /* for never write barriers */
  }
}

/*
 * Write barrier
 *   Paint obj(Black) to obj(Gray).
 *
 *   The object that is painted gray will be traversed atomically in final
 *   mark phase. So you use this write barrier if it's frequency written spot.
 *   e.g. Set element on Array.
 */

MRB_API void
mrb_write_barrier(mrb_state *mrb, struct RBasic *obj)
{
  mrb_gc *gc = &mrb->gc;

  if (!is_black(obj)) return;

  mrb_assert(!is_dead(gc, obj));
  mrb_assert(is_generational(gc) || gc->state != MRB_GC_STATE_ROOT);
  paint_gray(obj);
  obj->gcnext = gc->atomic_gray_list;
  gc->atomic_gray_list = obj;
}

/*
 *  call-seq:
 *     GC.start                     -> nil
 *
 *  Initiates full garbage collection.
 *
 */

static mrb_value
gc_start(mrb_state *mrb, mrb_value obj)
{
  mrb_full_gc(mrb);
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning <code>true</code> if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

static mrb_value
gc_enable(mrb_state *mrb, mrb_value obj)
{
  mrb_bool old = mrb->gc.disabled;

  mrb->gc.disabled = FALSE;

  return mrb_bool_value(old);
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning <code>true</code> if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

static mrb_value
gc_disable(mrb_state *mrb, mrb_value obj)
{
  mrb_bool old = mrb->gc.disabled;

  mrb->gc.disabled = TRUE;

  return mrb_bool_value(old);
}

/*
 *  call-seq:
 *     GC.interval_ratio      -> fixnum
 *
 *  Returns ratio of GC interval. Default value is 200(%).
 *
 */

static mrb_value
gc_interval_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_fixnum_value(mrb->gc.interval_ratio);
}

/*
 *  call-seq:
 *     GC.interval_ratio = fixnum    -> nil
 *
 *  Updates ratio of GC interval. Default value is 200(%).
 *  GC start as soon as after end all step of GC if you set 100(%).
 *
 */

static mrb_value
gc_interval_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc.interval_ratio = (int)ratio;
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.step_ratio    -> fixnum
 *
 *  Returns step span ratio of Incremental GC. Default value is 200(%).
 *
 */

static mrb_value
gc_step_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_fixnum_value(mrb->gc.step_ratio);
}

/*
 *  call-seq:
 *     GC.step_ratio = fixnum   -> nil
 *
 *  Updates step span ratio of Incremental GC. Default value is 200(%).
 *  1 step of incrementalGC becomes long if a rate is big.
 *
 */

static mrb_value
gc_step_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc.step_ratio = (int)ratio;
  return mrb_nil_value();
}

static void
change_gen_gc_mode(mrb_state *mrb, mrb_gc *gc, mrb_bool enable)
{
  if (gc->disabled || gc->iterating) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "generational mode changed when GC disabled");
    return;
  }
  if (is_generational(gc) && !enable) {
    clear_all_old(mrb, gc);
    mrb_assert(gc->state == MRB_GC_STATE_ROOT);
    gc->full = FALSE;
  }
  else if (!is_generational(gc) && enable) {
    incremental_gc_until(mrb, gc, MRB_GC_STATE_ROOT);
    gc->majorgc_old_threshold = gc->live_after_mark/100 * MAJOR_GC_INC_RATIO;
    gc->full = FALSE;
  }
  gc->generational = enable;
}

/*
 *  call-seq:
 *     GC.generational_mode -> true or false
 *
 *  Returns generational or normal gc mode.
 *
 */

static mrb_value
gc_generational_mode_get(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(mrb->gc.generational);
}

/*
 *  call-seq:
 *     GC.generational_mode = true or false -> true or false
 *
 *  Changes to generational or normal gc mode.
 *
 */

static mrb_value
gc_generational_mode_set(mrb_state *mrb, mrb_value self)
{
  mrb_bool enable;

  mrb_get_args(mrb, "b", &enable);
  if (mrb->gc.generational != enable)
    change_gen_gc_mode(mrb, &mrb->gc, enable);

  return mrb_bool_value(enable);
}


static void
gc_each_objects(mrb_state *mrb, mrb_gc *gc, mrb_each_object_callback *callback, void *data)
{
  mrb_heap_page* page;

  page = gc->heaps;
  while (page != NULL) {
    RVALUE *p;
    int i;

    p = objects(page);
    for (i=0; i < MRB_HEAP_PAGE_SIZE; i++) {
      if ((*callback)(mrb, &p[i].as.basic, data) == MRB_EACH_OBJ_BREAK)
        return;
    }
    page = page->next;
  }
}

void
mrb_objspace_each_objects(mrb_state *mrb, mrb_each_object_callback *callback, void *data)
{
  mrb_bool iterating = mrb->gc.iterating;

  mrb_full_gc(mrb);
  mrb->gc.iterating = TRUE;
  if (iterating) {
    gc_each_objects(mrb, &mrb->gc, callback, data);
  }
  else {
    struct mrb_jmpbuf *prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;

    MRB_TRY(&c_jmp) {
      mrb->jmp = &c_jmp;
      gc_each_objects(mrb, &mrb->gc, callback, data);
      mrb->jmp = prev_jmp;
      mrb->gc.iterating = iterating;
   } MRB_CATCH(&c_jmp) {
      mrb->gc.iterating = iterating;
      mrb->jmp = prev_jmp;
      MRB_THROW(prev_jmp);
    } MRB_END_EXC(&c_jmp);
  }
}

#ifdef GC_TEST
#ifdef GC_DEBUG
static mrb_value gc_test(mrb_state *, mrb_value);
#endif
#endif

void
mrb_init_gc(mrb_state *mrb)
{
  struct RClass *gc;

  mrb_static_assert(sizeof(RVALUE) <= sizeof(void*) * 6,
                    "RVALUE size must be within 6 words");

  gc = mrb_define_module(mrb, "GC");

  mrb_define_class_method(mrb, gc, "start", gc_start, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, gc, "enable", gc_enable, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, gc, "disable", gc_disable, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, gc, "interval_ratio", gc_interval_ratio_get, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, gc, "interval_ratio=", gc_interval_ratio_set, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, gc, "step_ratio", gc_step_ratio_get, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, gc, "step_ratio=", gc_step_ratio_set, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, gc, "generational_mode=", gc_generational_mode_set, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, gc, "generational_mode", gc_generational_mode_get, MRB_ARGS_NONE());
#ifdef GC_TEST
#ifdef GC_DEBUG
  mrb_define_class_method(mrb, gc, "test", gc_test, MRB_ARGS_NONE());
#endif
#endif
}
/*
** hash.c - Hash class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#ifndef MRB_WITHOUT_FLOAT
/* a function to get hash value of a float number */
mrb_int mrb_float_id(mrb_float f);
#endif

#ifndef MRB_HT_INIT_SIZE
#define MRB_HT_INIT_SIZE 4
#endif
#define HT_SEG_INCREASE_RATIO 6 / 5

struct segkv {
  mrb_value key;
  mrb_value val;
};

typedef struct hash_segment {
  uint16_t size;
  struct hash_segment *next;
  struct segkv e[];
} hash_segment;

typedef struct segindex {
  size_t size;
  size_t capa;
  struct segkv *table[];
} segindex;

/* hash table structure */
typedef struct htable {
  hash_segment *rootseg;
  hash_segment *lastseg;
  mrb_int size;
  uint16_t last_len;
  segindex *index;
} htable;

static /* inline */ size_t
ht_hash_func(mrb_state *mrb, htable *t, mrb_value key)
{
  enum mrb_vtype tt = mrb_type(key);
  mrb_value hv;
  size_t h;
  segindex *index = t->index;
  size_t capa = index ? index->capa : 0;

  switch (tt) {
  case MRB_TT_STRING:
    h = mrb_str_hash(mrb, key);
    break;

  case MRB_TT_TRUE:
  case MRB_TT_FALSE:
  case MRB_TT_SYMBOL:
  case MRB_TT_FIXNUM:
#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
#endif
    h = (size_t)mrb_obj_id(key);
    break;

  default:
    hv = mrb_funcall(mrb, key, "hash", 0);
    h = (size_t)t ^ (size_t)mrb_fixnum(hv);
    break;
  }
  if (index && (index != t->index || capa != index->capa)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "hash modified");
  }
  return ((h)^((h)<<2)^((h)>>2));
}

static inline mrb_bool
ht_hash_equal(mrb_state *mrb, htable *t, mrb_value a, mrb_value b)
{
  enum mrb_vtype tt = mrb_type(a);

  switch (tt) {
  case MRB_TT_STRING:
    return mrb_str_equal(mrb, a, b);

  case MRB_TT_SYMBOL:
    if (!mrb_symbol_p(b)) return FALSE;
    return mrb_symbol(a) == mrb_symbol(b);

  case MRB_TT_FIXNUM:
    switch (mrb_type(b)) {
    case MRB_TT_FIXNUM:
      return mrb_fixnum(a) == mrb_fixnum(b);
#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      return (mrb_float)mrb_fixnum(a) == mrb_float(b);
#endif
    default:
      return FALSE;
    }

#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
    switch (mrb_type(b)) {
    case MRB_TT_FIXNUM:
      return mrb_float(a) == (mrb_float)mrb_fixnum(b);
    case MRB_TT_FLOAT:
      return mrb_float(a) == mrb_float(b);
    default:
      return FALSE;
    }
#endif

  default:
    {
      segindex *index = t->index;
      size_t capa = index ? index->capa : 0;
      mrb_bool eql = mrb_eql(mrb, a, b);
      if (index && (index != t->index || capa != index->capa)) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "hash modified");
      }
      return eql;
    }
  } 
}

/* Creates the hash table. */
static htable*
ht_new(mrb_state *mrb)
{
  htable *t;

  t = (htable*)mrb_malloc(mrb, sizeof(htable));
  t->size = 0;
  t->rootseg =  NULL;
  t->lastseg =  NULL;
  t->last_len = 0;
  t->index = NULL;

  return t;
}

#define power2(v) do { \
  v--;\
  v |= v >> 1;\
  v |= v >> 2;\
  v |= v >> 4;\
  v |= v >> 8;\
  v |= v >> 16;\
  v++;\
} while (0)

#ifndef UPPER_BOUND
#define UPPER_BOUND(x) ((x)>>2|(x)>>1)
#endif

#define HT_MASK(index) ((index->capa)-1)

/* Build index for the hash table */
static void
ht_index(mrb_state *mrb, htable *t)
{
  size_t size = (size_t)t->size;
  size_t mask;
  segindex *index = t->index;
  hash_segment *seg;
  size_t i;

  /* allocate index table */
  if (index && index->size >= UPPER_BOUND(index->capa)) {
    size = index->capa+1;
  }
  power2(size);
  if (!index || index->capa < size) {
    index = (segindex*)mrb_realloc_simple(mrb, index, sizeof(segindex)+sizeof(struct segkv*)*size);
    if (index == NULL) {
      mrb_free(mrb, t->index);
      t->index = NULL;
      return;
    }
    t->index = index;
  }
  index->size = t->size;
  index->capa = size;
  for (i=0; i<size; i++) {
    index->table[i] = NULL;
  }

  /* rebuld index */
  mask = HT_MASK(index);
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value key;
      size_t k, step = 0;

      if (!seg->next && i >= (size_t)t->last_len) {
        return;
      }
      key = seg->e[i].key;
      if (mrb_undef_p(key)) continue;
      k = ht_hash_func(mrb, t, key) & mask;
      while (index->table[k]) {
        k = (k+(++step)) & mask;
      }
      index->table[k] = &seg->e[i];
    }
    seg = seg->next;
  }
}

/* Compacts the hash table removing deleted entries. */
static void
ht_compact(mrb_state *mrb, htable *t)
{
  hash_segment *seg;
  uint16_t i, i2;
  hash_segment *seg2 = NULL;
  mrb_int size = 0;

  if (t == NULL) return;
  seg = t->rootseg;
  if (t->index && (size_t)t->size == t->index->size) {
    ht_index(mrb, t);
    return;
  }
  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value k = seg->e[i].key;

      if (!seg->next && i >= t->last_len) {
        goto exit;
      }
      if (mrb_undef_p(k)) {     /* found deleted key */
        if (seg2 == NULL) {
          seg2 = seg;
          i2 = i;
        }
      }
      else {
        size++;
        if (seg2 != NULL) {
          seg2->e[i2++] = seg->e[i];
          if (i2 >= seg2->size) {
            seg2 = seg2->next;
            i2 = 0;
          }
        }
      }
    }
    seg = seg->next;
  }
 exit:
  /* reached at end */
  t->size = size;
  if (seg2) {
    seg = seg2->next;
    seg2->next = NULL;
    t->last_len = i2;
    t->lastseg = seg2;
    while (seg) {
      seg2 = seg->next;
      mrb_free(mrb, seg);
      seg = seg2;
    }
  }
  if (t->index) {
    ht_index(mrb, t);
  }
}

static hash_segment*
segment_alloc(mrb_state *mrb, hash_segment *seg)
{
  uint32_t size;

  if (!seg) size = MRB_HT_INIT_SIZE;
  else {
    size = seg->size*HT_SEG_INCREASE_RATIO + 1;
    if (size > UINT16_MAX) size = UINT16_MAX;
  }

  seg = (hash_segment*)mrb_malloc(mrb, sizeof(hash_segment)+sizeof(struct segkv)*size);
  seg->size = size;
  seg->next = NULL;

  return seg;
}

/* Set the value for the key in the indexed table. */
static void
ht_index_put(mrb_state *mrb, htable *t, mrb_value key, mrb_value val)
{
  segindex *index = t->index;
  size_t k, sp, step = 0, mask;
  hash_segment *seg;

  if (index->size >= UPPER_BOUND(index->capa)) {
    /* need to expand table */
    ht_compact(mrb, t);
    index = t->index;
  }
  mask = HT_MASK(index);
  sp = index->capa;
  k = ht_hash_func(mrb, t, key) & mask;
  while (index->table[k]) {
    mrb_value key2 = index->table[k]->key;
    if (mrb_undef_p(key2)) {
      if (sp == index->capa) sp = k;
    }
    else if (ht_hash_equal(mrb, t, key, key2)) {
      index->table[k]->val = val;
      return;
    }
    k = (k+(++step)) & mask;
  }
  if (sp < index->capa) {
    k = sp;
  }

  /* put the value at the last */
  seg = t->lastseg;
  if (t->last_len < seg->size) {
    index->table[k] = &seg->e[t->last_len++];
  }
  else {                        /* append a new hash_segment */
    seg->next = segment_alloc(mrb, seg);
    seg = seg->next;
    seg->next = NULL;
    t->lastseg = seg;
    t->last_len = 1;
    index->table[k] = &seg->e[0];
  }
  index->table[k]->key = key;
  index->table[k]->val = val;
  index->size++;
  t->size++;
}

/* Set the value for the key in the hash table. */
static void
ht_put(mrb_state *mrb, htable *t, mrb_value key, mrb_value val)
{
  hash_segment *seg;
  mrb_int i, deleted = 0;

  if (t == NULL) return;
  if (t->index) {
    ht_index_put(mrb, t, key, val);
    return;
  }
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value k = seg->e[i].key;
      /* Found room in last hash_segment after last_len */
      if (!seg->next && i >= t->last_len) {
        seg->e[i].key = key;
        seg->e[i].val = val;
        t->last_len = (uint16_t)i+1;
        t->size++;
        return;
      }
      if (mrb_undef_p(k)) {
        deleted++;
        continue;
      }
      if (ht_hash_equal(mrb, t, key, k)) {
        seg->e[i].val = val;
        return;
      }
    }
    seg = seg->next;
  }
  /* Not found */

  /* Compact if last hash_segment has room */
  if (deleted > 0 && deleted > MRB_HT_INIT_SIZE) {
    ht_compact(mrb, t);
  }
  t->size++;

  /* check if thre's room after compaction */
  if (t->lastseg && t->last_len < t->lastseg->size) {
    seg = t->lastseg;
    i = t->last_len;
  }
  else {
    /* append new hash_segment */
    seg = segment_alloc(mrb, t->lastseg);
    i = 0;
    if (t->rootseg == NULL) {
      t->rootseg = seg;
    }
    else {
      t->lastseg->next = seg;
    }
    t->lastseg = seg;
  }
  seg->e[i].key = key;
  seg->e[i].val = val;
  t->last_len = (uint16_t)i+1;
  if (t->index == NULL && t->size > MRB_HT_INIT_SIZE*4) {
    ht_index(mrb, t);
  }
}

/* Get a value for a key from the indexed table. */
static mrb_bool
ht_index_get(mrb_state *mrb, htable *t, mrb_value key, mrb_value *vp)
{
  segindex *index = t->index;
  size_t mask = HT_MASK(index);
  size_t k = ht_hash_func(mrb, t, key) & mask;
  size_t step = 0;

  while (index->table[k]) {
    mrb_value key2 = index->table[k]->key;
    if (!mrb_undef_p(key2) && ht_hash_equal(mrb, t, key, key2)) {
      if (vp) *vp = index->table[k]->val;
      return TRUE;
    }
    k = (k+(++step)) & mask;
  }
  return FALSE;
}

/* Get a value for a key from the hash table. */
static mrb_bool
ht_get(mrb_state *mrb, htable *t, mrb_value key, mrb_value *vp)
{
  hash_segment *seg;
  mrb_int i;

  if (t == NULL) return FALSE;
  if (t->index) {
    return ht_index_get(mrb, t, key, vp);
  }

  seg = t->rootseg;
  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value k = seg->e[i].key;

      if (!seg->next && i >= t->last_len) {
        return FALSE;
      }
      if (mrb_undef_p(k)) continue;
      if (ht_hash_equal(mrb, t, key, k)) {
        if (vp) *vp = seg->e[i].val;
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Deletes the value for the symbol from the hash table. */
/* Deletion is done by overwriting keys by `undef`. */
static mrb_bool
ht_del(mrb_state *mrb, htable *t, mrb_value key, mrb_value *vp)
{
  hash_segment *seg;
  mrb_int i;

  if (t == NULL) return FALSE;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value key2;

      if (!seg->next && i >= t->last_len) {
        /* not found */
        return FALSE;
      }
      key2 = seg->e[i].key;
      if (!mrb_undef_p(key2) && ht_hash_equal(mrb, t, key, key2)) {
        if (vp) *vp = seg->e[i].val;
        seg->e[i].key = mrb_undef_value();
        t->size--;
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Iterates over the hash table. */
static void
ht_foreach(mrb_state *mrb, htable *t, mrb_hash_foreach_func *func, void *p)
{
  hash_segment *seg;
  mrb_int i;

  if (t == NULL) return;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<seg->size; i++) {
      /* no value in last hash_segment after last_len */
      if (!seg->next && i >= t->last_len) {
        return;
      }
      if (mrb_undef_p(seg->e[i].key)) continue;
      if ((*func)(mrb, seg->e[i].key, seg->e[i].val, p) != 0)
        return;
    }
    seg = seg->next;
  }
}

/* Iterates over the hash table. */
MRB_API void
mrb_hash_foreach(mrb_state *mrb, struct RHash *hash, mrb_hash_foreach_func *func, void *p)
{
  ht_foreach(mrb, hash->ht, func, p);
}

/* Copy the hash table. */
static htable*
ht_copy(mrb_state *mrb, htable *t)
{
  hash_segment *seg;
  htable *t2;
  mrb_int i;

  seg = t->rootseg;
  t2 = ht_new(mrb);
  if (t->size == 0) return t2;

  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value key = seg->e[i].key;
      mrb_value val = seg->e[i].val;

      if ((seg->next == NULL) && (i >= t->last_len)) {
        return t2;
      }
      if (mrb_undef_p(key)) continue; /* skip deleted key */
      ht_put(mrb, t2, key, val);
    }
    seg = seg->next;
  }
  return t2;
}

/* Free memory of the hash table. */
static void
ht_free(mrb_state *mrb, htable *t)
{
  hash_segment *seg;

  if (!t) return;
  seg = t->rootseg;
  while (seg) {
    hash_segment *p = seg;
    seg = seg->next;
    mrb_free(mrb, p);
  }
  if (t->index) mrb_free(mrb, t->index);
  mrb_free(mrb, t);
}

static void mrb_hash_modify(mrb_state *mrb, mrb_value hash);

static inline mrb_value
ht_key(mrb_state *mrb, mrb_value key)
{
  if (mrb_string_p(key) && !mrb_frozen_p(mrb_str_ptr(key))) {
    key = mrb_str_dup(mrb, key);
    MRB_SET_FROZEN_FLAG(mrb_str_ptr(key));
  }
  return key;
}

#define KEY(key) ht_key(mrb, key)

static int
hash_mark_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_gc_mark_value(mrb, key);
  mrb_gc_mark_value(mrb, val);
  return 0;
}

void
mrb_gc_mark_hash(mrb_state *mrb, struct RHash *hash)
{
  ht_foreach(mrb, hash->ht, hash_mark_i, NULL);
}

size_t
mrb_gc_mark_hash_size(mrb_state *mrb, struct RHash *hash)
{
  if (!hash->ht) return 0;
  return hash->ht->size*2;
}

void
mrb_gc_free_hash(mrb_state *mrb, struct RHash *hash)
{
  ht_free(mrb, hash->ht);
}

MRB_API mrb_value
mrb_hash_new(mrb_state *mrb)
{
  struct RHash *h;

  h = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  h->ht = 0;
  h->iv = 0;
  return mrb_obj_value(h);
}

MRB_API mrb_value
mrb_hash_new_capa(mrb_state *mrb, mrb_int capa)
{
  struct RHash *h;

  h = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  /* preallocate hash table */
  h->ht = ht_new(mrb);
  /* capacity ignored */
  h->iv = 0;
  return mrb_obj_value(h);
}

static mrb_value mrb_hash_default(mrb_state *mrb, mrb_value hash);
static mrb_value hash_default(mrb_state *mrb, mrb_value hash, mrb_value key);

static mrb_value
mrb_hash_init_copy(mrb_state *mrb, mrb_value self)
{
  mrb_value orig = mrb_get_arg1(mrb);
  struct RHash* copy;
  htable *orig_h;
  mrb_value ifnone, vret;

  if (mrb_obj_equal(mrb, self, orig)) return self;
  if ((mrb_type(self) != mrb_type(orig)) || (mrb_obj_class(mrb, self) != mrb_obj_class(mrb, orig))) {
      mrb_raise(mrb, E_TYPE_ERROR, "initialize_copy should take same class object");
  }

  orig_h = RHASH_TBL(self);
  copy = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  copy->ht = ht_copy(mrb, orig_h);

  if (MRB_RHASH_DEFAULT_P(self)) {
    copy->flags |= MRB_HASH_DEFAULT;
  }
  if (MRB_RHASH_PROCDEFAULT_P(self)) {
    copy->flags |= MRB_HASH_PROC_DEFAULT;
  }
  vret = mrb_obj_value(copy);
  ifnone = RHASH_IFNONE(self);
  if (!mrb_nil_p(ifnone)) {
      mrb_iv_set(mrb, vret, mrb_intern_lit(mrb, "ifnone"), ifnone);
  }
  return vret;
}

static int
check_kdict_i(mrb_state *mrb, mrb_value key, mrb_value val, void *data)
{
  if (!mrb_symbol_p(key)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "keyword argument hash with non symbol keys");
  }
  return 0;
}

void
mrb_hash_check_kdict(mrb_state *mrb, mrb_value self)
{
  htable *t;

  t = RHASH_TBL(self);
  if (!t || t->size == 0) return;
  ht_foreach(mrb, t, check_kdict_i, NULL);
}

MRB_API mrb_value
mrb_hash_dup(mrb_state *mrb, mrb_value self)
{
  struct RHash* copy;
  htable *orig_h;

  orig_h = RHASH_TBL(self);
  copy = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  copy->ht = orig_h ? ht_copy(mrb, orig_h) : NULL;
  return mrb_obj_value(copy);
}

MRB_API mrb_value
mrb_hash_get(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  mrb_value val;
  mrb_sym mid;

  if (ht_get(mrb, RHASH_TBL(hash), key, &val)) {
    return val;
  }

  mid = mrb_intern_lit(mrb, "default");
  if (mrb_func_basic_p(mrb, hash, mid, mrb_hash_default)) {
    return hash_default(mrb, hash, key);
  }
  /* xxx mrb_funcall_tailcall(mrb, hash, "default", 1, key); */
  return mrb_funcall_argv(mrb, hash, mid, 1, &key);
}

MRB_API mrb_value
mrb_hash_fetch(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value def)
{
  mrb_value val;

  if (ht_get(mrb, RHASH_TBL(hash), key, &val)) {
    return val;
  }
  /* not found */
  return def;
}

MRB_API void
mrb_hash_set(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value val)
{
  mrb_hash_modify(mrb, hash);

  key = KEY(key);
  ht_put(mrb, RHASH_TBL(hash), key, val);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)RHASH(hash), key);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)RHASH(hash), val);
  return;
}

static void
mrb_hash_modify(mrb_state *mrb, mrb_value hash)
{
  mrb_check_frozen(mrb, mrb_hash_ptr(hash));
  if (!RHASH_TBL(hash)) {
    RHASH_TBL(hash) = ht_new(mrb);
  }
}

/* 15.2.13.4.16 */
/*
 *  call-seq:
 *     Hash.new                          -> new_hash
 *     Hash.new(obj)                     -> new_hash
 *     Hash.new {|hash, key| block }     -> new_hash
 *
 *  Returns a new, empty hash. If this hash is subsequently accessed by
 *  a key that doesn't correspond to a hash entry, the value returned
 *  depends on the style of <code>new</code> used to create the hash. In
 *  the first form, the access returns <code>nil</code>. If
 *  <i>obj</i> is specified, this single object will be used for
 *  all <em>default values</em>. If a block is specified, it will be
 *  called with the hash object and the key, and should return the
 *  default value. It is the block's responsibility to store the value
 *  in the hash if required.
 *
 *      h = Hash.new("Go Fish")
 *      h["a"] = 100
 *      h["b"] = 200
 *      h["a"]           #=> 100
 *      h["c"]           #=> "Go Fish"
 *      # The following alters the single default object
 *      h["c"].upcase!   #=> "GO FISH"
 *      h["d"]           #=> "GO FISH"
 *      h.keys           #=> ["a", "b"]
 *
 *      # While this creates a new default object each time
 *      h = Hash.new { |hash, key| hash[key] = "Go Fish: #{key}" }
 *      h["c"]           #=> "Go Fish: c"
 *      h["c"].upcase!   #=> "GO FISH: C"
 *      h["d"]           #=> "Go Fish: d"
 *      h.keys           #=> ["c", "d"]
 *
 */

static mrb_value
mrb_hash_init(mrb_state *mrb, mrb_value hash)
{
  mrb_value block, ifnone;
  mrb_bool ifnone_p;

  ifnone = mrb_nil_value();
  mrb_get_args(mrb, "&|o?", &block, &ifnone, &ifnone_p);
  mrb_hash_modify(mrb, hash);
  if (!mrb_nil_p(block)) {
    if (ifnone_p) {
      mrb_argnum_error(mrb, 1, 0, 0);
    }
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    ifnone = block;
  }
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
    mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  }
  return hash;
}

/* 15.2.13.4.2  */
/*
 *  call-seq:
 *     hsh[key]    ->  value
 *
 *  Element Reference---Retrieves the <i>value</i> object corresponding
 *  to the <i>key</i> object. If not found, returns the default value (see
 *  <code>Hash::new</code> for details).
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h["a"]   #=> 100
 *     h["c"]   #=> nil
 *
 */
static mrb_value
mrb_hash_aget(mrb_state *mrb, mrb_value self)
{
  mrb_value key = mrb_get_arg1(mrb);

  return mrb_hash_get(mrb, self, key);
}

static mrb_value
hash_default(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.5  */
/*
 *  call-seq:
 *     hsh.default(key=nil)   -> obj
 *
 *  Returns the default value, the value that would be returned by
 *  <i>hsh</i>[<i>key</i>] if <i>key</i> did not exist in <i>hsh</i>.
 *  See also <code>Hash::new</code> and <code>Hash#default=</code>.
 *
 *     h = Hash.new                            #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> nil
 *
 *     h = Hash.new("cat")                     #=> {}
 *     h.default                               #=> "cat"
 *     h.default(2)                            #=> "cat"
 *
 *     h = Hash.new {|h,k| h[k] = k.to_i*10}   #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> 20
 */

static mrb_value
mrb_hash_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value key;
  mrb_bool given;

  mrb_get_args(mrb, "|o?", &key, &given);
  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      if (!given) return mrb_nil_value();
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.6  */
/*
 *  call-seq:
 *     hsh.default = obj     -> obj
 *
 *  Sets the default value, the value returned for a key that does not
 *  exist in the hash. It is not possible to set the default to a
 *  <code>Proc</code> that will be executed on each key lookup.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.default = "Go fish"
 *     h["a"]     #=> 100
 *     h["z"]     #=> "Go fish"
 *     # This doesn't do what you might hope...
 *     h.default = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> #<Proc:0x401b3948@-:6>
 *     h["cat"]   #=> #<Proc:0x401b3948@-:6>
 */

static mrb_value
mrb_hash_set_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone = mrb_get_arg1(mrb);

  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  RHASH(hash)->flags &= ~MRB_HASH_PROC_DEFAULT;
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
  }
  else {
    RHASH(hash)->flags &= ~MRB_HASH_DEFAULT;
  }
  return ifnone;
}

/* 15.2.13.4.7  */
/*
 *  call-seq:
 *     hsh.default_proc -> anObject
 *
 *  If <code>Hash::new</code> was invoked with a block, return that
 *  block, otherwise return <code>nil</code>.
 *
 *     h = Hash.new {|h,k| h[k] = k*k }   #=> {}
 *     p = h.default_proc                 #=> #<Proc:0x401b3d08@-:1>
 *     a = []                             #=> []
 *     p.call(a, 2)
 *     a                                  #=> [nil, nil, 4]
 */


static mrb_value
mrb_hash_default_proc(mrb_state *mrb, mrb_value hash)
{
  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    return RHASH_PROCDEFAULT(hash);
  }
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     hsh.default_proc = proc_obj     -> proc_obj
 *
 *  Sets the default proc to be executed on each key lookup.
 *
 *     h.default_proc = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> 4
 *     h["cat"]   #=> "catcat"
 */

static mrb_value
mrb_hash_set_default_proc(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone = mrb_get_arg1(mrb);

  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
  }
  else {
    RHASH(hash)->flags &= ~MRB_HASH_DEFAULT;
    RHASH(hash)->flags &= ~MRB_HASH_PROC_DEFAULT;
  }

  return ifnone;
}

MRB_API mrb_value
mrb_hash_delete_key(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  htable *t = RHASH_TBL(hash);
  mrb_value del_val;

  if (ht_del(mrb, t, key, &del_val)) {
    return del_val;
  }

  /* not found */
  return mrb_nil_value();
}

static mrb_value
mrb_hash_delete(mrb_state *mrb, mrb_value self)
{
  mrb_value key = mrb_get_arg1(mrb);

  mrb_hash_modify(mrb, self);
  return mrb_hash_delete_key(mrb, self, key);
}

/* find first element in the hash table, and remove it. */
static void
ht_shift(mrb_state *mrb, htable *t, mrb_value *kp, mrb_value *vp)
{
  hash_segment *seg = t->rootseg;
  mrb_int i;

  while (seg) {
    for (i=0; i<seg->size; i++) {
      mrb_value key;

      if (!seg->next && i >= t->last_len) {
        return;
      }
      key = seg->e[i].key;
      if (mrb_undef_p(key)) continue;
      *kp = key;
      *vp = seg->e[i].val;
      /* delete element */
      seg->e[i].key = mrb_undef_value();
      t->size--;
      return;
    }
    seg = seg->next;
  }
}

/* 15.2.13.4.24 */
/*
 *  call-seq:
 *     hsh.shift -> anArray or obj
 *
 *  Removes a key-value pair from <i>hsh</i> and returns it as the
 *  two-item array <code>[</code> <i>key, value</i> <code>]</code>, or
 *  the hash's default value if the hash is empty.
 *
 *      h = { 1 => "a", 2 => "b", 3 => "c" }
 *      h.shift   #=> [1, "a"]
 *      h         #=> {2=>"b", 3=>"c"}
 */

static mrb_value
mrb_hash_shift(mrb_state *mrb, mrb_value hash)
{
  htable *t = RHASH_TBL(hash);

  mrb_hash_modify(mrb, hash);
  if (t && t->size > 0) {
    mrb_value del_key, del_val;

    ht_shift(mrb, t, &del_key, &del_val);
    mrb_gc_protect(mrb, del_key);
    mrb_gc_protect(mrb, del_val);
    return mrb_assoc_new(mrb, del_key, del_val);
  }

  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, mrb_nil_value());
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.4  */
/*
 *  call-seq:
 *     hsh.clear -> hsh
 *
 *  Removes all key-value pairs from `hsh`.
 *
 *      h = { "a" => 100, "b" => 200 }   #=> {"a"=>100, "b"=>200}
 *      h.clear                          #=> {}
 *
 */

MRB_API mrb_value
mrb_hash_clear(mrb_state *mrb, mrb_value hash)
{
  htable *t = RHASH_TBL(hash);

  mrb_hash_modify(mrb, hash);
  if (t) {
    ht_free(mrb, t);
    RHASH_TBL(hash) = NULL;
  }
  return hash;
}

/* 15.2.13.4.3  */
/* 15.2.13.4.26 */
/*
 *  call-seq:
 *     hsh[key] = value        -> value
 *     hsh.store(key, value)   -> value
 *
 *  Element Assignment---Associates the value given by
 *  <i>value</i> with the key given by <i>key</i>.
 *  <i>key</i> should not have its value changed while it is in
 *  use as a key (a <code>String</code> passed as a key will be
 *  duplicated and frozen).
 *
 *      h = { "a" => 100, "b" => 200 }
 *      h["a"] = 9
 *      h["c"] = 4
 *      h   #=> {"a"=>9, "b"=>200, "c"=>4}
 *
 */
static mrb_value
mrb_hash_aset(mrb_state *mrb, mrb_value self)
{
  mrb_value key, val;

  mrb_get_args(mrb, "oo", &key, &val);
  mrb_hash_set(mrb, self, key, val);
  return val;
}

MRB_API mrb_int
mrb_hash_size(mrb_state *mrb, mrb_value hash)
{
  htable *t = RHASH_TBL(hash);

  if (!t) return 0;
  return t->size;
}

/* 15.2.13.4.20 */
/* 15.2.13.4.25 */
/*
 *  call-seq:
 *     hsh.length    ->  fixnum
 *     hsh.size      ->  fixnum
 *
 *  Returns the number of key-value pairs in the hash.
 *
 *     h = { "d" => 100, "a" => 200, "v" => 300, "e" => 400 }
 *     h.length        #=> 4
 *     h.delete("a")   #=> 200
 *     h.length        #=> 3
 */
static mrb_value
mrb_hash_size_m(mrb_state *mrb, mrb_value self)
{
  mrb_int size = mrb_hash_size(mrb, self);
  return mrb_fixnum_value(size);
}

MRB_API mrb_bool
mrb_hash_empty_p(mrb_state *mrb, mrb_value self)
{
  htable *t = RHASH_TBL(self);

  if (!t) return TRUE;
  return t->size == 0;
}

/* 15.2.13.4.12 */
/*
 *  call-seq:
 *     hsh.empty?    -> true or false
 *
 *  Returns <code>true</code> if <i>hsh</i> contains no key-value pairs.
 *
 *     {}.empty?   #=> true
 *
 */
static mrb_value
mrb_hash_empty_m(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(mrb_hash_empty_p(mrb, self));
}

static int
hash_keys_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_ary_push(mrb, *(mrb_value*)p, key);
  return 0;
}

/* 15.2.13.4.19 */
/*
 *  call-seq:
 *     hsh.keys    -> array
 *
 *  Returns a new array populated with the keys from this hash. See also
 *  <code>Hash#values</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300, "d" => 400 }
 *     h.keys   #=> ["a", "b", "c", "d"]
 *
 */

MRB_API mrb_value
mrb_hash_keys(mrb_state *mrb, mrb_value hash)
{
  htable *t = RHASH_TBL(hash);
  mrb_int size;
  mrb_value ary;

  if (!t || (size = t->size) == 0)
    return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, size);
  ht_foreach(mrb, t, hash_keys_i, (void*)&ary);
  return ary;
}

static int
hash_vals_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_ary_push(mrb, *(mrb_value*)p, val);
  return 0;
}

/* 15.2.13.4.28 */
/*
 *  call-seq:
 *     hsh.values    -> array
 *
 *  Returns a new array populated with the values from <i>hsh</i>. See
 *  also <code>Hash#keys</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300 }
 *     h.values   #=> [100, 200, 300]
 *
 */

MRB_API mrb_value
mrb_hash_values(mrb_state *mrb, mrb_value hash)
{
  htable *t = RHASH_TBL(hash);
  mrb_int size;
  mrb_value ary;

  if (!t || (size = t->size) == 0)
    return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, size);
  ht_foreach(mrb, t, hash_vals_i, (void*)&ary);
  return ary;
}

/* 15.2.13.4.13 */
/* 15.2.13.4.15 */
/* 15.2.13.4.18 */
/* 15.2.13.4.21 */
/*
 *  call-seq:
 *     hsh.has_key?(key)    -> true or false
 *     hsh.include?(key)    -> true or false
 *     hsh.key?(key)        -> true or false
 *     hsh.member?(key)     -> true or false
 *
 *  Returns <code>true</code> if the given key is present in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_key?("a")   #=> true
 *     h.has_key?("z")   #=> false
 *
 */

MRB_API mrb_bool
mrb_hash_key_p(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  htable *t;

  t = RHASH_TBL(hash);
  if (ht_get(mrb, t, key, NULL)) {
    return TRUE;
  }
  return FALSE;
}

static mrb_value
mrb_hash_has_key(mrb_state *mrb, mrb_value hash)
{
  mrb_value key = mrb_get_arg1(mrb);
  mrb_bool key_p;

  key_p = mrb_hash_key_p(mrb, hash, key);
  return mrb_bool_value(key_p);
}

struct has_v_arg {
  mrb_bool found;
  mrb_value val;
};

static int
hash_has_value_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  struct has_v_arg *arg = (struct has_v_arg*)p;
  
  if (mrb_equal(mrb, arg->val, val)) {
    arg->found = TRUE;
    return 1;
  }
  return 0;
}

/* 15.2.13.4.14 */
/* 15.2.13.4.27 */
/*
 *  call-seq:
 *     hsh.has_value?(value)    -> true or false
 *     hsh.value?(value)        -> true or false
 *
 *  Returns <code>true</code> if the given value is present for some key
 *  in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_value?(100)   #=> true
 *     h.has_value?(999)   #=> false
 */

static mrb_value
mrb_hash_has_value(mrb_state *mrb, mrb_value hash)
{
  mrb_value val = mrb_get_arg1(mrb);
  struct has_v_arg arg;
  
  arg.found = FALSE;
  arg.val = val;
  ht_foreach(mrb, RHASH_TBL(hash), hash_has_value_i, &arg);
  return mrb_bool_value(arg.found);
}

static int
merge_i(mrb_state *mrb, mrb_value key, mrb_value val, void *data)
{
  htable *h1 = (htable*)data;

  ht_put(mrb, h1, key, val);
  return 0;
}

MRB_API void
mrb_hash_merge(mrb_state *mrb, mrb_value hash1, mrb_value hash2)
{
  htable *h1, *h2;

  mrb_hash_modify(mrb, hash1);
  hash2 = mrb_ensure_hash_type(mrb, hash2);
  h1 = RHASH_TBL(hash1);
  h2 = RHASH_TBL(hash2);

  if (!h2) return;
  if (!h1) {
    RHASH_TBL(hash1) = ht_copy(mrb, h2);
    return;
  }
  ht_foreach(mrb, h2, merge_i, h1);
  mrb_write_barrier(mrb, (struct RBasic*)RHASH(hash1));
  return;
}

/*
 *  call-seq:
 *    hsh.rehash -> hsh
 *
 *  Rebuilds the hash based on the current hash values for each key. If
 *  values of key objects have changed since they were inserted, this
 *  method will reindex <i>hsh</i>.
 *
 *     keys = (1..17).map{|n| [n]}
 *     k = keys[0]
 *     h = {}
 *     keys.each{|key| h[key] = key[0]}
 *     h     #=> { [1]=> 1, [2]=> 2, [3]=> 3, [4]=> 4, [5]=> 5, [6]=> 6, [7]=> 7,
 *                 [8]=> 8, [9]=> 9,[10]=>10,[11]=>11,[12]=>12,[13]=>13,[14]=>14,
 *                [15]=>15,[16]=>16,[17]=>17}
 *     h[k]  #=> 1
 *     k[0] = keys.size + 1
 *     h     #=> {[18]=> 1, [2]=> 2, [3]=> 3, [4]=> 4, [5]=> 5, [6]=> 6, [7]=> 7,
 *                 [8]=> 8, [9]=> 9,[10]=>10,[11]=>11,[12]=>12,[13]=>13,[14]=>14,
 *                [15]=>15,[16]=>16,[17]=>17}
 *     h[k]  #=> nil
 *     h.rehash
 *     h[k]  #=> 1
 */
static mrb_value
mrb_hash_rehash(mrb_state *mrb, mrb_value self)
{
  ht_compact(mrb, RHASH_TBL(self));
  return self;
}

void
mrb_init_hash(mrb_state *mrb)
{
  struct RClass *h;

  mrb->hash_class = h = mrb_define_class(mrb, "Hash", mrb->object_class);              /* 15.2.13 */
  MRB_SET_INSTANCE_TT(h, MRB_TT_HASH);

  mrb_define_method(mrb, h, "initialize_copy", mrb_hash_init_copy,   MRB_ARGS_REQ(1));
  mrb_define_method(mrb, h, "[]",              mrb_hash_aget,        MRB_ARGS_REQ(1)); /* 15.2.13.4.2  */
  mrb_define_method(mrb, h, "[]=",             mrb_hash_aset,        MRB_ARGS_REQ(2)); /* 15.2.13.4.3  */
  mrb_define_method(mrb, h, "clear",           mrb_hash_clear,       MRB_ARGS_NONE()); /* 15.2.13.4.4  */
  mrb_define_method(mrb, h, "default",         mrb_hash_default,     MRB_ARGS_OPT(1));  /* 15.2.13.4.5  */
  mrb_define_method(mrb, h, "default=",        mrb_hash_set_default, MRB_ARGS_REQ(1)); /* 15.2.13.4.6  */
  mrb_define_method(mrb, h, "default_proc",    mrb_hash_default_proc,MRB_ARGS_NONE()); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "default_proc=",   mrb_hash_set_default_proc,MRB_ARGS_REQ(1)); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "__delete",        mrb_hash_delete,      MRB_ARGS_REQ(1)); /* core of 15.2.13.4.8  */
  mrb_define_method(mrb, h, "empty?",          mrb_hash_empty_m,     MRB_ARGS_NONE()); /* 15.2.13.4.12 */
  mrb_define_method(mrb, h, "has_key?",        mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.13 */
  mrb_define_method(mrb, h, "has_value?",      mrb_hash_has_value,   MRB_ARGS_REQ(1)); /* 15.2.13.4.14 */
  mrb_define_method(mrb, h, "include?",        mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.15 */
  mrb_define_method(mrb, h, "initialize",      mrb_hash_init,        MRB_ARGS_OPT(1)|MRB_ARGS_BLOCK()); /* 15.2.13.4.16 */
  mrb_define_method(mrb, h, "key?",            mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.18 */
  mrb_define_method(mrb, h, "keys",            mrb_hash_keys,        MRB_ARGS_NONE()); /* 15.2.13.4.19 */
  mrb_define_method(mrb, h, "length",          mrb_hash_size_m,      MRB_ARGS_NONE()); /* 15.2.13.4.20 */
  mrb_define_method(mrb, h, "member?",         mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.21 */
  mrb_define_method(mrb, h, "shift",           mrb_hash_shift,       MRB_ARGS_NONE()); /* 15.2.13.4.24 */
  mrb_define_method(mrb, h, "size",            mrb_hash_size_m,      MRB_ARGS_NONE()); /* 15.2.13.4.25 */
  mrb_define_method(mrb, h, "store",           mrb_hash_aset,        MRB_ARGS_REQ(2)); /* 15.2.13.4.26 */
  mrb_define_method(mrb, h, "value?",          mrb_hash_has_value,   MRB_ARGS_REQ(1)); /* 15.2.13.4.27 */
  mrb_define_method(mrb, h, "values",          mrb_hash_values,      MRB_ARGS_NONE()); /* 15.2.13.4.28 */
  mrb_define_method(mrb, h, "rehash",          mrb_hash_rehash,      MRB_ARGS_NONE());
}
/*
** init.c - initialize mruby core
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>

void mrb_init_symtbl(mrb_state*);
void mrb_init_class(mrb_state*);
void mrb_init_object(mrb_state*);
void mrb_init_kernel(mrb_state*);
void mrb_init_comparable(mrb_state*);
void mrb_init_enumerable(mrb_state*);
void mrb_init_symbol(mrb_state*);
void mrb_init_string(mrb_state*);
void mrb_init_exception(mrb_state*);
void mrb_init_proc(mrb_state*);
void mrb_init_array(mrb_state*);
void mrb_init_hash(mrb_state*);
void mrb_init_numeric(mrb_state*);
void mrb_init_range(mrb_state*);
void mrb_init_gc(mrb_state*);
void mrb_init_math(mrb_state*);
void mrb_init_version(mrb_state*);
void mrb_init_mrblib(mrb_state*);

#define DONE mrb_gc_arena_restore(mrb, 0);
void
mrb_init_core(mrb_state *mrb)
{
  mrb_init_symtbl(mrb); DONE;

  mrb_init_class(mrb); DONE;
  mrb_init_object(mrb); DONE;
  mrb_init_kernel(mrb); DONE;
  mrb_init_comparable(mrb); DONE;
  mrb_init_enumerable(mrb); DONE;

  mrb_init_symbol(mrb); DONE;
  mrb_init_string(mrb); DONE;
  mrb_init_exception(mrb); DONE;
  mrb_init_proc(mrb); DONE;
  mrb_init_array(mrb); DONE;
  mrb_init_hash(mrb); DONE;
  mrb_init_numeric(mrb); DONE;
  mrb_init_range(mrb); DONE;
  mrb_init_gc(mrb); DONE;
  mrb_init_version(mrb); DONE;
  mrb_init_mrblib(mrb); DONE;
}
/*
** kernel.c - Kernel module
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/istruct.h>

MRB_API mrb_bool
mrb_func_basic_p(mrb_state *mrb, mrb_value obj, mrb_sym mid, mrb_func_t func)
{
  struct RClass *c = mrb_class(mrb, obj);
  mrb_method_t m = mrb_method_search_vm(mrb, &c, mid);
  struct RProc *p;

  if (MRB_METHOD_UNDEF_P(m)) return FALSE;
  if (MRB_METHOD_FUNC_P(m))
    return MRB_METHOD_FUNC(m) == func;
  p = MRB_METHOD_PROC(m);
  if (MRB_PROC_CFUNC_P(p) && (MRB_PROC_CFUNC(p) == func))
    return TRUE;
  return FALSE;
}

static mrb_bool
mrb_obj_basic_to_s_p(mrb_state *mrb, mrb_value obj)
{
  return mrb_func_basic_p(mrb, obj, mrb_intern_lit(mrb, "to_s"), mrb_any_to_s);
}

/* 15.3.1.3.17 */
/*
 *  call-seq:
 *     obj.inspect   -> string
 *
 *  Returns a string containing a human-readable representation of
 *  <i>obj</i>. If not overridden and no instance variables, uses the
 *  <code>to_s</code> method to generate the string.
 *  <i>obj</i>.  If not overridden, uses the <code>to_s</code> method to
 *  generate the string.
 *
 *     [ 1, 2, 3..4, 'five' ].inspect   #=> "[1, 2, 3..4, \"five\"]"
 *     Time.new.inspect                 #=> "2008-03-08 19:43:39 +0900"
 */
MRB_API mrb_value
mrb_obj_inspect(mrb_state *mrb, mrb_value obj)
{
  if (mrb_object_p(obj) && mrb_obj_basic_to_s_p(mrb, obj)) {
    return mrb_obj_iv_inspect(mrb, mrb_obj_ptr(obj));
  }
  return mrb_any_to_s(mrb, obj);
}

/* 15.3.1.3.2  */
/*
 *  call-seq:
 *     obj === other   -> true or false
 *
 *  Case Equality---For class <code>Object</code>, effectively the same
 *  as calling  <code>#==</code>, but typically overridden by descendants
 *  to provide meaningful semantics in <code>case</code> statements.
 */
static mrb_value
mrb_equal_m(mrb_state *mrb, mrb_value self)
{
  mrb_value arg = mrb_get_arg1(mrb);

  return mrb_bool_value(mrb_equal(mrb, self, arg));
}

/* 15.3.1.3.3  */
/* 15.3.1.3.33 */
/*
 *  Document-method: __id__
 *  Document-method: object_id
 *
 *  call-seq:
 *     obj.__id__       -> fixnum
 *     obj.object_id    -> fixnum
 *
 *  Returns an integer identifier for <i>obj</i>. The same number will
 *  be returned on all calls to <code>id</code> for a given object, and
 *  no two active objects will share an id.
 *  <code>Object#object_id</code> is a different concept from the
 *  <code>:name</code> notation, which returns the symbol id of
 *  <code>name</code>. Replaces the deprecated <code>Object#id</code>.
 */
mrb_value
mrb_obj_id_m(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(mrb_obj_id(self));
}

/* 15.3.1.2.2  */
/* 15.3.1.2.5  */
/* 15.3.1.3.6  */
/* 15.3.1.3.25 */
/*
 *  call-seq:
 *     block_given?   -> true or false
 *     iterator?      -> true or false
 *
 *  Returns <code>true</code> if <code>yield</code> would execute a
 *  block in the current context. The <code>iterator?</code> form
 *  is mildly deprecated.
 *
 *     def try
 *       if block_given?
 *         yield
 *       else
 *         "no block"
 *       end
 *     end
 *     try                  #=> "no block"
 *     try { "hello" }      #=> "hello"
 *     try do "hello" end   #=> "hello"
 */
static mrb_value
mrb_f_block_given_p_m(mrb_state *mrb, mrb_value self)
{
  mrb_callinfo *ci = &mrb->c->ci[-1];
  mrb_callinfo *cibase = mrb->c->cibase;
  mrb_value *bp;
  struct RProc *p;

  if (ci <= cibase) {
    /* toplevel does not have block */
    return mrb_false_value();
  }
  p = ci->proc;
  /* search method/class/module proc */
  while (p) {
    if (MRB_PROC_SCOPE_P(p)) break;
    p = p->upper;
  }
  if (p == NULL) return mrb_false_value();
  /* search ci corresponding to proc */
  while (cibase < ci) {
    if (ci->proc == p) break;
    ci--;
  }
  if (ci == cibase) {
    return mrb_false_value();
  }
  else if (ci->env) {
    struct REnv *e = ci->env;
    int bidx;

    /* top-level does not have block slot (always false) */
    if (e->stack == mrb->c->stbase)
      return mrb_false_value();
    /* use saved block arg position */
    bidx = MRB_ENV_BIDX(e);
    /* bidx may be useless (e.g. define_method) */
    if (bidx >= MRB_ENV_STACK_LEN(e))
      return mrb_false_value();
    bp = &e->stack[bidx];
  }
  else {
    bp = ci[1].stackent+1;
    if (ci->argc >= 0) {
      bp += ci->argc;
    }
    else {
      bp++;
    }
  }
  if (mrb_nil_p(*bp))
    return mrb_false_value();
  return mrb_true_value();
}

/* 15.3.1.3.7  */
/*
 *  call-seq:
 *     obj.class    -> class
 *
 *  Returns the class of <i>obj</i>. This method must always be
 *  called with an explicit receiver, as <code>class</code> is also a
 *  reserved word in Ruby.
 *
 *     1.class      #=> Fixnum
 *     self.class   #=> Object
 */
static mrb_value
mrb_obj_class_m(mrb_state *mrb, mrb_value self)
{
  return mrb_obj_value(mrb_obj_class(mrb, self));
}

static struct RClass*
mrb_singleton_class_clone(mrb_state *mrb, mrb_value obj)
{
  struct RClass *klass = mrb_basic_ptr(obj)->c;

  if (klass->tt != MRB_TT_SCLASS)
    return klass;
  else {
    /* copy singleton(unnamed) class */
    struct RClass *clone = (struct RClass*)mrb_obj_alloc(mrb, klass->tt, mrb->class_class);

    switch (mrb_type(obj)) {
    case MRB_TT_CLASS:
    case MRB_TT_SCLASS:
      break;
    default:
      clone->c = mrb_singleton_class_clone(mrb, mrb_obj_value(klass));
      break;
    }
    clone->super = klass->super;
    if (klass->iv) {
      mrb_iv_copy(mrb, mrb_obj_value(clone), mrb_obj_value(klass));
      mrb_obj_iv_set(mrb, (struct RObject*)clone, mrb_intern_lit(mrb, "__attached__"), obj);
    }
    if (klass->mt) {
      clone->mt = kh_copy(mt, mrb, klass->mt);
    }
    else {
      clone->mt = kh_init(mt, mrb);
    }
    clone->tt = MRB_TT_SCLASS;
    return clone;
  }
}

static void
copy_class(mrb_state *mrb, mrb_value dst, mrb_value src)
{
  struct RClass *dc = mrb_class_ptr(dst);
  struct RClass *sc = mrb_class_ptr(src);
  /* if the origin is not the same as the class, then the origin and
     the current class need to be copied */
  if (sc->flags & MRB_FL_CLASS_IS_PREPENDED) {
    struct RClass *c0 = sc->super;
    struct RClass *c1 = dc;

    /* copy prepended iclasses */
    while (!(c0->flags & MRB_FL_CLASS_IS_ORIGIN)) {
      c1->super = mrb_class_ptr(mrb_obj_dup(mrb, mrb_obj_value(c0)));
      c1 = c1->super;
      c0 = c0->super;
    }
    c1->super = mrb_class_ptr(mrb_obj_dup(mrb, mrb_obj_value(c0)));
    c1->super->flags |= MRB_FL_CLASS_IS_ORIGIN;
  }
  if (sc->mt) {
    dc->mt = kh_copy(mt, mrb, sc->mt);
  }
  else {
    dc->mt = kh_init(mt, mrb);
  }
  dc->super = sc->super;
  MRB_SET_INSTANCE_TT(dc, MRB_INSTANCE_TT(sc));
}

static void
init_copy(mrb_state *mrb, mrb_value dest, mrb_value obj)
{
  switch (mrb_type(obj)) {
    case MRB_TT_ICLASS:
      copy_class(mrb, dest, obj);
      return;
    case MRB_TT_CLASS:
    case MRB_TT_MODULE:
      copy_class(mrb, dest, obj);
      mrb_iv_copy(mrb, dest, obj);
      mrb_iv_remove(mrb, dest, mrb_intern_lit(mrb, "__classname__"));
      break;
    case MRB_TT_OBJECT:
    case MRB_TT_SCLASS:
    case MRB_TT_HASH:
    case MRB_TT_DATA:
    case MRB_TT_EXCEPTION:
      mrb_iv_copy(mrb, dest, obj);
      break;
    case MRB_TT_ISTRUCT:
      mrb_istruct_copy(dest, obj);
      break;

    default:
      break;
  }
  mrb_funcall(mrb, dest, "initialize_copy", 1, obj);
}

/* 15.3.1.3.8  */
/*
 *  call-seq:
 *     obj.clone -> an_object
 *
 *  Produces a shallow copy of <i>obj</i>---the instance variables of
 *  <i>obj</i> are copied, but not the objects they reference. Copies
 *  the frozen state of <i>obj</i>. See also the discussion
 *  under <code>Object#dup</code>.
 *
 *     class Klass
 *        attr_accessor :str
 *     end
 *     s1 = Klass.new      #=> #<Klass:0x401b3a38>
 *     s1.str = "Hello"    #=> "Hello"
 *     s2 = s1.clone       #=> #<Klass:0x401b3998 @str="Hello">
 *     s2.str[1,4] = "i"   #=> "i"
 *     s1.inspect          #=> "#<Klass:0x401b3a38 @str=\"Hi\">"
 *     s2.inspect          #=> "#<Klass:0x401b3998 @str=\"Hi\">"
 *
 *  This method may have class-specific behavior.  If so, that
 *  behavior will be documented under the #+initialize_copy+ method of
 *  the class.
 *
 *  Some Class(True False Nil Symbol Fixnum Float) Object  cannot clone.
 */
MRB_API mrb_value
mrb_obj_clone(mrb_state *mrb, mrb_value self)
{
  struct RObject *p;
  mrb_value clone;

  if (mrb_immediate_p(self)) {
    return self;
  }
  if (mrb_sclass_p(self)) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't clone singleton class");
  }
  p = (struct RObject*)mrb_obj_alloc(mrb, mrb_type(self), mrb_obj_class(mrb, self));
  p->c = mrb_singleton_class_clone(mrb, self);
  mrb_field_write_barrier(mrb, (struct RBasic*)p, (struct RBasic*)p->c);
  clone = mrb_obj_value(p);
  init_copy(mrb, clone, self);
  p->flags |= mrb_obj_ptr(self)->flags & MRB_FL_OBJ_IS_FROZEN;

  return clone;
}

/* 15.3.1.3.9  */
/*
 *  call-seq:
 *     obj.dup -> an_object
 *
 *  Produces a shallow copy of <i>obj</i>---the instance variables of
 *  <i>obj</i> are copied, but not the objects they reference.
 *  <code>dup</code> copies the frozen state of <i>obj</i>. See also
 *  the discussion under <code>Object#clone</code>. In general,
 *  <code>clone</code> and <code>dup</code> may have different semantics
 *  in descendant classes. While <code>clone</code> is used to duplicate
 *  an object, including its internal state, <code>dup</code> typically
 *  uses the class of the descendant object to create the new instance.
 *
 *  This method may have class-specific behavior.  If so, that
 *  behavior will be documented under the #+initialize_copy+ method of
 *  the class.
 */

MRB_API mrb_value
mrb_obj_dup(mrb_state *mrb, mrb_value obj)
{
  struct RBasic *p;
  mrb_value dup;

  if (mrb_immediate_p(obj)) {
    return obj;
  }
  if (mrb_sclass_p(obj)) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't dup singleton class");
  }
  p = mrb_obj_alloc(mrb, mrb_type(obj), mrb_obj_class(mrb, obj));
  dup = mrb_obj_value(p);
  init_copy(mrb, dup, obj);

  return dup;
}

static mrb_value
mrb_obj_extend(mrb_state *mrb, mrb_int argc, mrb_value *argv, mrb_value obj)
{
  mrb_int i;

  if (argc == 0) {
    mrb_argnum_error(mrb, argc, 1, -1);
  }
  for (i = 0; i < argc; i++) {
    mrb_check_type(mrb, argv[i], MRB_TT_MODULE);
  }
  while (argc--) {
    mrb_funcall(mrb, argv[argc], "extend_object", 1, obj);
    mrb_funcall(mrb, argv[argc], "extended", 1, obj);
  }
  return obj;
}

/* 15.3.1.3.13 */
/*
 *  call-seq:
 *     obj.extend(module, ...)    -> obj
 *
 *  Adds to _obj_ the instance methods from each module given as a
 *  parameter.
 *
 *     module Mod
 *       def hello
 *         "Hello from Mod.\n"
 *       end
 *     end
 *
 *     class Klass
 *       def hello
 *         "Hello from Klass.\n"
 *       end
 *     end
 *
 *     k = Klass.new
 *     k.hello         #=> "Hello from Klass.\n"
 *     k.extend(Mod)   #=> #<Klass:0x401b3bc8>
 *     k.hello         #=> "Hello from Mod.\n"
 */
static mrb_value
mrb_obj_extend_m(mrb_state *mrb, mrb_value self)
{
  mrb_value *argv;
  mrb_int argc;

  mrb_get_args(mrb, "*", &argv, &argc);
  return mrb_obj_extend(mrb, argc, argv, self);
}

MRB_API mrb_value
mrb_obj_freeze(mrb_state *mrb, mrb_value self)
{
  if (!mrb_immediate_p(self)) {
    struct RBasic *b = mrb_basic_ptr(self);
    if (!mrb_frozen_p(b)) {
      MRB_SET_FROZEN_FLAG(b);
      if (b->c->tt == MRB_TT_SCLASS) MRB_SET_FROZEN_FLAG(b->c);
    }
  }
  return self;
}

static mrb_value
mrb_obj_frozen(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(mrb_immediate_p(self) || mrb_frozen_p(mrb_basic_ptr(self)));
}

/* 15.3.1.3.15 */
/*
 *  call-seq:
 *     obj.hash    -> fixnum
 *
 *  Generates a <code>Fixnum</code> hash value for this object. This
 *  function must have the property that <code>a.eql?(b)</code> implies
 *  <code>a.hash == b.hash</code>. The hash value is used by class
 *  <code>Hash</code>. Any hash value that exceeds the capacity of a
 *  <code>Fixnum</code> will be truncated before being used.
 */
static mrb_value
mrb_obj_hash(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(mrb_obj_id(self));
}

/* 15.3.1.3.16 */
static mrb_value
mrb_obj_init_copy(mrb_state *mrb, mrb_value self)
{
  mrb_value orig = mrb_get_arg1(mrb);

  if (mrb_obj_equal(mrb, self, orig)) return self;
  if ((mrb_type(self) != mrb_type(orig)) || (mrb_obj_class(mrb, self) != mrb_obj_class(mrb, orig))) {
      mrb_raise(mrb, E_TYPE_ERROR, "initialize_copy should take same class object");
  }
  return self;
}


MRB_API mrb_bool
mrb_obj_is_instance_of(mrb_state *mrb, mrb_value obj, struct RClass* c)
{
  if (mrb_obj_class(mrb, obj) == c) return TRUE;
  return FALSE;
}

/* 15.3.1.3.19 */
/*
 *  call-seq:
 *     obj.instance_of?(class)    -> true or false
 *
 *  Returns <code>true</code> if <i>obj</i> is an instance of the given
 *  class. See also <code>Object#kind_of?</code>.
 */
static mrb_value
obj_is_instance_of(mrb_state *mrb, mrb_value self)
{
  mrb_value arg;

  mrb_get_args(mrb, "C", &arg);

  return mrb_bool_value(mrb_obj_is_instance_of(mrb, self, mrb_class_ptr(arg)));
}

/* 15.3.1.3.24 */
/* 15.3.1.3.26 */
/*
 *  call-seq:
 *     obj.is_a?(class)       -> true or false
 *     obj.kind_of?(class)    -> true or false
 *
 *  Returns <code>true</code> if <i>class</i> is the class of
 *  <i>obj</i>, or if <i>class</i> is one of the superclasses of
 *  <i>obj</i> or modules included in <i>obj</i>.
 *
 *     module M;    end
 *     class A
 *       include M
 *     end
 *     class B < A; end
 *     class C < B; end
 *     b = B.new
 *     b.instance_of? A   #=> false
 *     b.instance_of? B   #=> true
 *     b.instance_of? C   #=> false
 *     b.instance_of? M   #=> false
 *     b.kind_of? A       #=> true
 *     b.kind_of? B       #=> true
 *     b.kind_of? C       #=> false
 *     b.kind_of? M       #=> true
 */
static mrb_value
mrb_obj_is_kind_of_m(mrb_state *mrb, mrb_value self)
{
  mrb_value arg;

  mrb_get_args(mrb, "C", &arg);

  return mrb_bool_value(mrb_obj_is_kind_of(mrb, self, mrb_class_ptr(arg)));
}

KHASH_DECLARE(st, mrb_sym, char, FALSE)
KHASH_DEFINE(st, mrb_sym, char, FALSE, kh_int_hash_func, kh_int_hash_equal)

/* 15.3.1.3.32 */
/*
 * call_seq:
 *   nil.nil?               -> true
 *   <anything_else>.nil?   -> false
 *
 * Only the object <i>nil</i> responds <code>true</code> to <code>nil?</code>.
 */
static mrb_value
mrb_false(mrb_state *mrb, mrb_value self)
{
  return mrb_false_value();
}

/* 15.3.1.2.12  */
/* 15.3.1.3.40 */
/*
 *  call-seq:
 *     raise
 *     raise(string)
 *     raise(exception [, string])
 *
 *  With no arguments, raises a <code>RuntimeError</code>
 *  With a single +String+ argument, raises a
 *  +RuntimeError+ with the string as a message. Otherwise,
 *  the first parameter should be the name of an +Exception+
 *  class (or an object that returns an +Exception+ object when sent
 *  an +exception+ message). The optional second parameter sets the
 *  message associated with the exception, and the third parameter is an
 *  array of callback information. Exceptions are caught by the
 *  +rescue+ clause of <code>begin...end</code> blocks.
 *
 *     raise "Failed to create socket"
 *     raise ArgumentError, "No parameters", caller
 */
MRB_API mrb_value
mrb_f_raise(mrb_state *mrb, mrb_value self)
{
  mrb_value a[2], exc;
  mrb_int argc;


  argc = mrb_get_args(mrb, "|oo", &a[0], &a[1]);
  switch (argc) {
  case 0:
    mrb_raise(mrb, E_RUNTIME_ERROR, "");
    break;
  case 1:
    if (mrb_string_p(a[0])) {
      a[1] = a[0];
      argc = 2;
      a[0] = mrb_obj_value(E_RUNTIME_ERROR);
    }
    /* fall through */
  default:
    exc = mrb_make_exception(mrb, argc, a);
    mrb_exc_raise(mrb, exc);
    break;
  }
  return mrb_nil_value();            /* not reached */
}

/* 15.3.1.3.41 */
/*
 *  call-seq:
 *     obj.remove_instance_variable(symbol)    -> obj
 *
 *  Removes the named instance variable from <i>obj</i>, returning that
 *  variable's value.
 *
 *     class Dummy
 *       attr_reader :var
 *       def initialize
 *         @var = 99
 *       end
 *       def remove
 *         remove_instance_variable(:@var)
 *       end
 *     end
 *     d = Dummy.new
 *     d.var      #=> 99
 *     d.remove   #=> 99
 *     d.var      #=> nil
 */
static mrb_value
mrb_obj_remove_instance_variable(mrb_state *mrb, mrb_value self)
{
  mrb_sym sym;
  mrb_value val;

  mrb_get_args(mrb, "n", &sym);
  mrb_iv_name_sym_check(mrb, sym);
  val = mrb_iv_remove(mrb, self, sym);
  if (mrb_undef_p(val)) {
    mrb_name_error(mrb, sym, "instance variable %n not defined", sym);
  }
  return val;
}

void
mrb_method_missing(mrb_state *mrb, mrb_sym name, mrb_value self, mrb_value args)
{
  mrb_no_method_error(mrb, name, args, "undefined method '%n'", name);
}

/* 15.3.1.3.30 */
/*
 *  call-seq:
 *     obj.method_missing(symbol [, *args] )   -> result
 *
 *  Invoked by Ruby when <i>obj</i> is sent a message it cannot handle.
 *  <i>symbol</i> is the symbol for the method called, and <i>args</i>
 *  are any arguments that were passed to it. By default, the interpreter
 *  raises an error when this method is called. However, it is possible
 *  to override the method to provide more dynamic behavior.
 *  If it is decided that a particular method should not be handled, then
 *  <i>super</i> should be called, so that ancestors can pick up the
 *  missing method.
 *  The example below creates
 *  a class <code>Roman</code>, which responds to methods with names
 *  consisting of roman numerals, returning the corresponding integer
 *  values.
 *
 *     class Roman
 *       def romanToInt(str)
 *         # ...
 *       end
 *       def method_missing(methId)
 *         str = methId.id2name
 *         romanToInt(str)
 *       end
 *     end
 *
 *     r = Roman.new
 *     r.iv      #=> 4
 *     r.xxiii   #=> 23
 *     r.mm      #=> 2000
 */
static mrb_value
mrb_obj_missing(mrb_state *mrb, mrb_value mod)
{
  mrb_sym name;
  mrb_value *a;
  mrb_int alen;

  mrb_get_args(mrb, "n*!", &name, &a, &alen);
  mrb_method_missing(mrb, name, mod, mrb_ary_new_from_values(mrb, alen, a));
  /* not reached */
  return mrb_nil_value();
}

static inline mrb_bool
basic_obj_respond_to(mrb_state *mrb, mrb_value obj, mrb_sym id, int pub)
{
  return mrb_respond_to(mrb, obj, id);
}

/* 15.3.1.3.43 */
/*
 *  call-seq:
 *     obj.respond_to?(symbol, include_private=false) -> true or false
 *
 *  Returns +true+ if _obj_ responds to the given
 *  method. Private methods are included in the search only if the
 *  optional second parameter evaluates to +true+.
 *
 *  If the method is not implemented,
 *  as Process.fork on Windows, File.lchmod on GNU/Linux, etc.,
 *  false is returned.
 *
 *  If the method is not defined, <code>respond_to_missing?</code>
 *  method is called and the result is returned.
 */
static mrb_value
obj_respond_to(mrb_state *mrb, mrb_value self)
{
  mrb_sym id, rtm_id;
  mrb_bool priv = FALSE, respond_to_p;

  mrb_get_args(mrb, "n|b", &id, &priv);
  respond_to_p = basic_obj_respond_to(mrb, self, id, !priv);
  if (!respond_to_p) {
    rtm_id = mrb_intern_lit(mrb, "respond_to_missing?");
    if (basic_obj_respond_to(mrb, self, rtm_id, !priv)) {
      mrb_value args[2], v;
      args[0] = mrb_symbol_value(id);
      args[1] = mrb_bool_value(priv);
      v = mrb_funcall_argv(mrb, self, rtm_id, 2, args);
      return mrb_bool_value(mrb_bool(v));
    }
  }
  return mrb_bool_value(respond_to_p);
}

static mrb_value
mrb_obj_ceqq(mrb_state *mrb, mrb_value self)
{
  mrb_value v = mrb_get_arg1(mrb);
  mrb_int i, len;
  mrb_sym eqq = mrb_intern_lit(mrb, "===");
  mrb_value ary = mrb_ary_splat(mrb, self);

  len = RARRAY_LEN(ary);
  for (i=0; i<len; i++) {
    mrb_value c = mrb_funcall_argv(mrb, mrb_ary_entry(ary, i), eqq, 1, &v);
    if (mrb_test(c)) return mrb_true_value();
  }
  return mrb_false_value();
}

mrb_value mrb_obj_equal_m(mrb_state *mrb, mrb_value);

void
mrb_init_kernel(mrb_state *mrb)
{
  struct RClass *krn;

  mrb->kernel_module = krn = mrb_define_module(mrb, "Kernel");                                                    /* 15.3.1 */
  mrb_define_class_method(mrb, krn, "block_given?",         mrb_f_block_given_p_m,           MRB_ARGS_NONE());    /* 15.3.1.2.2  */
  mrb_define_class_method(mrb, krn, "iterator?",            mrb_f_block_given_p_m,           MRB_ARGS_NONE());    /* 15.3.1.2.5  */
;     /* 15.3.1.2.11 */
  mrb_define_class_method(mrb, krn, "raise",                mrb_f_raise,                     MRB_ARGS_OPT(2));    /* 15.3.1.2.12 */


  mrb_define_method(mrb, krn, "===",                        mrb_equal_m,                     MRB_ARGS_REQ(1));    /* 15.3.1.3.2  */
  mrb_define_method(mrb, krn, "block_given?",               mrb_f_block_given_p_m,           MRB_ARGS_NONE());    /* 15.3.1.3.6  */
  mrb_define_method(mrb, krn, "class",                      mrb_obj_class_m,                 MRB_ARGS_NONE());    /* 15.3.1.3.7  */
  mrb_define_method(mrb, krn, "clone",                      mrb_obj_clone,                   MRB_ARGS_NONE());    /* 15.3.1.3.8  */
  mrb_define_method(mrb, krn, "dup",                        mrb_obj_dup,                     MRB_ARGS_NONE());    /* 15.3.1.3.9  */
  mrb_define_method(mrb, krn, "eql?",                       mrb_obj_equal_m,                 MRB_ARGS_REQ(1));    /* 15.3.1.3.10 */
  mrb_define_method(mrb, krn, "extend",                     mrb_obj_extend_m,                MRB_ARGS_ANY());     /* 15.3.1.3.13 */
  mrb_define_method(mrb, krn, "freeze",                     mrb_obj_freeze,                  MRB_ARGS_NONE());
  mrb_define_method(mrb, krn, "frozen?",                    mrb_obj_frozen,                  MRB_ARGS_NONE());
  mrb_define_method(mrb, krn, "hash",                       mrb_obj_hash,                    MRB_ARGS_NONE());    /* 15.3.1.3.15 */
  mrb_define_method(mrb, krn, "initialize_copy",            mrb_obj_init_copy,               MRB_ARGS_REQ(1));    /* 15.3.1.3.16 */
  mrb_define_method(mrb, krn, "inspect",                    mrb_obj_inspect,                 MRB_ARGS_NONE());    /* 15.3.1.3.17 */
  mrb_define_method(mrb, krn, "instance_of?",               obj_is_instance_of,              MRB_ARGS_REQ(1));    /* 15.3.1.3.19 */

  mrb_define_method(mrb, krn, "is_a?",                      mrb_obj_is_kind_of_m,            MRB_ARGS_REQ(1));    /* 15.3.1.3.24 */
  mrb_define_method(mrb, krn, "iterator?",                  mrb_f_block_given_p_m,           MRB_ARGS_NONE());    /* 15.3.1.3.25 */
  mrb_define_method(mrb, krn, "kind_of?",                   mrb_obj_is_kind_of_m,            MRB_ARGS_REQ(1));    /* 15.3.1.3.26 */
  mrb_define_method(mrb, krn, "method_missing",             mrb_obj_missing,                 MRB_ARGS_ANY());     /* 15.3.1.3.30 */
  mrb_define_method(mrb, krn, "nil?",                       mrb_false,                       MRB_ARGS_NONE());    /* 15.3.1.3.32 */
  mrb_define_method(mrb, krn, "object_id",                  mrb_obj_id_m,                    MRB_ARGS_NONE());    /* 15.3.1.3.33 */
  mrb_define_method(mrb, krn, "raise",                      mrb_f_raise,                     MRB_ARGS_ANY());     /* 15.3.1.3.40 */
  mrb_define_method(mrb, krn, "remove_instance_variable",   mrb_obj_remove_instance_variable,MRB_ARGS_REQ(1));    /* 15.3.1.3.41 */
  mrb_define_method(mrb, krn, "respond_to?",                obj_respond_to,                  MRB_ARGS_ARG(1,1));     /* 15.3.1.3.43 */
  mrb_define_method(mrb, krn, "to_s",                       mrb_any_to_s,                    MRB_ARGS_NONE());    /* 15.3.1.3.46 */
  mrb_define_method(mrb, krn, "__case_eqq",                 mrb_obj_ceqq,                    MRB_ARGS_REQ(1));    /* internal */
  mrb_define_method(mrb, krn, "__to_int",                   mrb_to_int,                      MRB_ARGS_NONE()); /* internal */
  mrb_define_method(mrb, krn, "__to_str",                   mrb_to_str,                      MRB_ARGS_NONE()); /* internal */

  mrb_include_module(mrb, mrb->object_class, mrb->kernel_module);
}
/*
** load.c - mruby binary loader
**
** See Copyright Notice in mruby.h
*/

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mruby/dump.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/debug.h>
#include <mruby/error.h>
#include <mruby/data.h>

#if SIZE_MAX < UINT32_MAX
# error size_t must be at least 32 bits wide
#endif

#define FLAG_SRC_MALLOC 1
#define FLAG_SRC_STATIC 0

#define SIZE_ERROR_MUL(nmemb, size) ((size_t)(nmemb) > SIZE_MAX / (size))

static size_t
skip_padding(const uint8_t *buf)
{
  const size_t align = MRB_DUMP_ALIGNMENT;
  return -(intptr_t)buf & (align-1);
}

static size_t
offset_crc_body(void)
{
  struct rite_binary_header header;
  return ((uint8_t *)header.binary_crc - (uint8_t *)&header) + sizeof(header.binary_crc);
}

#ifndef MRB_WITHOUT_FLOAT
double mrb_str_len_to_dbl(mrb_state *mrb, const char *s, size_t len, mrb_bool badcheck);

static double
str_to_double(mrb_state *mrb, const char *p, size_t len)
{
  /* `i`, `inf`, `infinity` */
  if (len > 0 && p[0] == 'i') return INFINITY;

  /* `I`, `-inf`, `-infinity` */
  if (p[0] == 'I' || (len > 1 && p[0] == '-' && p[1] == 'i')) return -INFINITY;
  return mrb_str_len_to_dbl(mrb, p, len, TRUE);
}
#endif

mrb_value mrb_str_len_to_inum(mrb_state *mrb, const char *str, mrb_int len, mrb_int base, int badcheck);

static void
load_tempirep_free(mrb_state *mrb, void *p)
{
  if (p) mrb_irep_decref(mrb, (mrb_irep *)p);
}

static const mrb_data_type load_tempirep_type = { "temporary irep", load_tempirep_free };

static mrb_irep*
read_irep_record_1(mrb_state *mrb, const uint8_t *bin, size_t *len, uint8_t flags)
{
  int i;
  const uint8_t *src = bin;
  ptrdiff_t diff;
  uint16_t tt, pool_data_len, snl;
  int plen;
  struct RData *irep_obj = mrb_data_object_alloc(mrb, mrb->object_class, NULL, &load_tempirep_type);
  mrb_irep *irep = mrb_add_irep(mrb);
  int ai = mrb_gc_arena_save(mrb);

  irep_obj->data = irep;

  /* skip record size */
  src += sizeof(uint32_t);

  /* number of local variable */
  irep->nlocals = bin_to_uint16(src);
  src += sizeof(uint16_t);

  /* number of register variable */
  irep->nregs = bin_to_uint16(src);
  src += sizeof(uint16_t);

  /* number of child irep */
  irep->rlen = (size_t)bin_to_uint16(src);
  src += sizeof(uint16_t);

  /* Binary Data Section */
  /* ISEQ BLOCK */
  irep->ilen = (uint16_t)bin_to_uint32(src);
  src += sizeof(uint32_t);
  src += skip_padding(src);

  if (irep->ilen > 0) {
    if (SIZE_ERROR_MUL(irep->ilen, sizeof(mrb_code))) {
      return NULL;
    }
    if ((flags & FLAG_SRC_MALLOC) == 0) {
      irep->iseq = (mrb_code*)src;
      src += sizeof(mrb_code) * irep->ilen;
      irep->flags |= MRB_ISEQ_NO_FREE;
    }
    else {
      size_t data_len = sizeof(mrb_code) * irep->ilen;
      void *buf = mrb_malloc(mrb, data_len);
      irep->iseq = (mrb_code *)buf;
      memcpy(buf, src, data_len);
      src += data_len;
    }
  }

  /* POOL BLOCK */
  plen = bin_to_uint32(src); /* number of pool */
  src += sizeof(uint32_t);
  if (plen > 0) {
    if (SIZE_ERROR_MUL(plen, sizeof(mrb_value))) {
      return NULL;
    }
    irep->pool = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value) * plen);

    for (i = 0; i < plen; i++) {
      const char *s;
      mrb_bool st = (flags & FLAG_SRC_MALLOC)==0;

      tt = *src++; /* pool TT */
      pool_data_len = bin_to_uint16(src); /* pool data length */
      src += sizeof(uint16_t);
      s = (const char*)src;
      src += pool_data_len;
      switch (tt) { /* pool data */
      case IREP_TT_FIXNUM: {
        mrb_value num = mrb_str_len_to_inum(mrb, s, pool_data_len, 10, FALSE);
#ifdef MRB_WITHOUT_FLOAT
        irep->pool[i] = num;
#else
        irep->pool[i] = mrb_float_p(num)? mrb_float_pool(mrb, mrb_float(num)) : num;
#endif
        }
        break;

#ifndef MRB_WITHOUT_FLOAT
      case IREP_TT_FLOAT:
        irep->pool[i] = mrb_float_pool(mrb, str_to_double(mrb, s, pool_data_len));
        break;
#endif

      case IREP_TT_STRING:
        irep->pool[i] = mrb_str_pool(mrb, s, pool_data_len, st);
        break;

      default:
        /* should not happen */
        irep->pool[i] = mrb_nil_value();
        break;
      }
      irep->plen++;
      mrb_gc_arena_restore(mrb, ai);
    }
  }

  /* SYMS BLOCK */
  irep->slen = (uint16_t)bin_to_uint32(src);  /* syms length */
  src += sizeof(uint32_t);
  if (irep->slen > 0) {
    if (SIZE_ERROR_MUL(irep->slen, sizeof(mrb_sym))) {
      return NULL;
    }
    irep->syms = (mrb_sym *)mrb_malloc(mrb, sizeof(mrb_sym) * irep->slen);

    for (i = 0; i < irep->slen; i++) {
      snl = bin_to_uint16(src);               /* symbol name length */
      src += sizeof(uint16_t);

      if (snl == MRB_DUMP_NULL_SYM_LEN) {
        irep->syms[i] = 0;
        continue;
      }

      if (flags & FLAG_SRC_MALLOC) {
        irep->syms[i] = mrb_intern(mrb, (char *)src, snl);
      }
      else {
        irep->syms[i] = mrb_intern_static(mrb, (char *)src, snl);
      }
      src += snl + 1;

      mrb_gc_arena_restore(mrb, ai);
    }
  }

  irep->reps = (mrb_irep**)mrb_calloc(mrb, irep->rlen, sizeof(mrb_irep*));

  diff = src - bin;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  *len = (size_t)diff;

  irep_obj->data = NULL;

  return irep;
}

static mrb_irep*
read_irep_record(mrb_state *mrb, const uint8_t *bin, size_t *len, uint8_t flags)
{
  struct RData *irep_obj = mrb_data_object_alloc(mrb, mrb->object_class, NULL, &load_tempirep_type);
  int ai = mrb_gc_arena_save(mrb);
  mrb_irep *irep = read_irep_record_1(mrb, bin, len, flags);
  int i;

  mrb_gc_arena_restore(mrb, ai);
  if (irep == NULL) {
    return NULL;
  }

  irep_obj->data = irep;

  bin += *len;
  for (i=0; i<irep->rlen; i++) {
    size_t rlen;

    irep->reps[i] = read_irep_record(mrb, bin, &rlen, flags);
    mrb_gc_arena_restore(mrb, ai);
    if (irep->reps[i] == NULL) {
      return NULL;
    }
    bin += rlen;
    *len += rlen;
  }

  irep_obj->data = NULL;

  return irep;
}

static mrb_irep*
read_section_irep(mrb_state *mrb, const uint8_t *bin, uint8_t flags)
{
  size_t len;

  bin += sizeof(struct rite_section_irep_header);
  return read_irep_record(mrb, bin, &len, flags);
}

static int
read_debug_record(mrb_state *mrb, const uint8_t *start, mrb_irep* irep, size_t *record_len, const mrb_sym *filenames, size_t filenames_len)
{
  const uint8_t *bin = start;
  ptrdiff_t diff;
  size_t record_size;
  uint16_t f_idx;
  int i;

  if (irep->debug_info) { return MRB_DUMP_INVALID_IREP; }

  irep->debug_info = (mrb_irep_debug_info*)mrb_calloc(mrb, 1, sizeof(mrb_irep_debug_info));
  irep->debug_info->pc_count = (uint32_t)irep->ilen;

  record_size = (size_t)bin_to_uint32(bin);
  bin += sizeof(uint32_t);

  irep->debug_info->flen = bin_to_uint16(bin);
  irep->debug_info->files = (mrb_irep_debug_info_file**)mrb_calloc(mrb, irep->debug_info->flen, sizeof(mrb_irep_debug_info*));
  bin += sizeof(uint16_t);

  for (f_idx = 0; f_idx < irep->debug_info->flen; ++f_idx) {
    mrb_irep_debug_info_file *file;
    uint16_t filename_idx;

    file = (mrb_irep_debug_info_file *)mrb_calloc(mrb, 1, sizeof(*file));
    irep->debug_info->files[f_idx] = file;

    file->start_pos = bin_to_uint32(bin);
    bin += sizeof(uint32_t);

    /* filename */
    filename_idx = bin_to_uint16(bin);
    bin += sizeof(uint16_t);
    mrb_assert(filename_idx < filenames_len);
    file->filename_sym = filenames[filename_idx];

    file->line_entry_count = bin_to_uint32(bin);
    bin += sizeof(uint32_t);
    file->line_type = (mrb_debug_line_type)bin_to_uint8(bin);
    bin += sizeof(uint8_t);
    switch (file->line_type) {
      case mrb_debug_line_ary: {
        uint32_t l;

        file->lines.ary = (uint16_t *)mrb_malloc(mrb, sizeof(uint16_t) * (size_t)(file->line_entry_count));
        for (l = 0; l < file->line_entry_count; ++l) {
          file->lines.ary[l] = bin_to_uint16(bin);
          bin += sizeof(uint16_t);
        }
      } break;

      case mrb_debug_line_flat_map: {
        uint32_t l;

        file->lines.flat_map = (mrb_irep_debug_info_line*)mrb_calloc(
            mrb, (size_t)(file->line_entry_count), sizeof(mrb_irep_debug_info_line));
        for (l = 0; l < file->line_entry_count; ++l) {
          file->lines.flat_map[l].start_pos = bin_to_uint32(bin);
          bin += sizeof(uint32_t);
          file->lines.flat_map[l].line = bin_to_uint16(bin);
          bin += sizeof(uint16_t);
        }
      } break;

      default: return MRB_DUMP_GENERAL_FAILURE;
    }
  }

  diff = bin - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);

  if (record_size != (size_t)diff) {
    return MRB_DUMP_GENERAL_FAILURE;
  }

  for (i = 0; i < irep->rlen; i++) {
    size_t len;
    int ret;

    ret = read_debug_record(mrb, bin, irep->reps[i], &len, filenames, filenames_len);
    if (ret != MRB_DUMP_OK) return ret;
    bin += len;
  }

  diff = bin - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  *record_len = (size_t)diff;

  return MRB_DUMP_OK;
}

static int
read_section_debug(mrb_state *mrb, const uint8_t *start, mrb_irep *irep, uint8_t flags)
{
  const uint8_t *bin;
  ptrdiff_t diff;
  struct rite_section_debug_header *header;
  uint16_t i;
  size_t len = 0;
  int result;
  uint16_t filenames_len;
  mrb_sym *filenames;
  mrb_value filenames_obj;

  bin = start;
  header = (struct rite_section_debug_header *)bin;
  bin += sizeof(struct rite_section_debug_header);

  filenames_len = bin_to_uint16(bin);
  bin += sizeof(uint16_t);
  filenames_obj = mrb_str_new(mrb, NULL, sizeof(mrb_sym) * (size_t)filenames_len);
  filenames = (mrb_sym*)RSTRING_PTR(filenames_obj);
  for (i = 0; i < filenames_len; ++i) {
    uint16_t f_len = bin_to_uint16(bin);
    bin += sizeof(uint16_t);
    if (flags & FLAG_SRC_MALLOC) {
      filenames[i] = mrb_intern(mrb, (const char *)bin, (size_t)f_len);
    }
    else {
      filenames[i] = mrb_intern_static(mrb, (const char *)bin, (size_t)f_len);
    }
    bin += f_len;
  }

  result = read_debug_record(mrb, bin, irep, &len, filenames, filenames_len);
  if (result != MRB_DUMP_OK) goto debug_exit;

  bin += len;
  diff = bin - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  if ((uint32_t)diff != bin_to_uint32(header->section_size)) {
    result = MRB_DUMP_GENERAL_FAILURE;
  }

debug_exit:
  mrb_str_resize(mrb, filenames_obj, 0);
  return result;
}

static int
read_lv_record(mrb_state *mrb, const uint8_t *start, mrb_irep *irep, size_t *record_len, mrb_sym const *syms, uint32_t syms_len)
{
  const uint8_t *bin = start;
  ptrdiff_t diff;
  int i;

  irep->lv = (struct mrb_locals*)mrb_malloc(mrb, sizeof(struct mrb_locals) * (irep->nlocals - 1));

  for (i = 0; i + 1< irep->nlocals; ++i) {
    uint16_t const sym_idx = bin_to_uint16(bin);
    bin += sizeof(uint16_t);
    if (sym_idx == RITE_LV_NULL_MARK) {
      irep->lv[i].name = 0;
      irep->lv[i].r = 0;
    }
    else {
      if (sym_idx >= syms_len) {
        return MRB_DUMP_GENERAL_FAILURE;
      }
      irep->lv[i].name = syms[sym_idx];

      irep->lv[i].r = bin_to_uint16(bin);
    }
    bin += sizeof(uint16_t);
  }

  for (i = 0; i < irep->rlen; ++i) {
    size_t len;
    int ret;

    ret = read_lv_record(mrb, bin, irep->reps[i], &len, syms, syms_len);
    if (ret != MRB_DUMP_OK) return ret;
    bin += len;
  }

  diff = bin - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  *record_len = (size_t)diff;

  return MRB_DUMP_OK;
}

static int
read_section_lv(mrb_state *mrb, const uint8_t *start, mrb_irep *irep, uint8_t flags)
{
  const uint8_t *bin;
  ptrdiff_t diff;
  struct rite_section_lv_header const *header;
  uint32_t i;
  size_t len = 0;
  int result;
  uint32_t syms_len;
  mrb_sym *syms;
  mrb_value syms_obj;
  mrb_sym (*intern_func)(mrb_state*, const char*, size_t) =
    (flags & FLAG_SRC_MALLOC)? mrb_intern : mrb_intern_static;

  bin = start;
  header = (struct rite_section_lv_header const*)bin;
  bin += sizeof(struct rite_section_lv_header);

  syms_len = bin_to_uint32(bin);
  bin += sizeof(uint32_t);
  syms_obj = mrb_str_new(mrb, NULL, sizeof(mrb_sym) * (size_t)syms_len);
  syms = (mrb_sym*)RSTRING_PTR(syms_obj);
  for (i = 0; i < syms_len; ++i) {
    uint16_t const str_len = bin_to_uint16(bin);
    bin += sizeof(uint16_t);

    syms[i] = intern_func(mrb, (const char*)bin, str_len);
    bin += str_len;
  }

  result = read_lv_record(mrb, bin, irep, &len, syms, syms_len);
  if (result != MRB_DUMP_OK) goto lv_exit;

  bin += len;
  diff = bin - start;
  mrb_assert_int_fit(ptrdiff_t, diff, size_t, SIZE_MAX);
  if ((uint32_t)diff != bin_to_uint32(header->section_size)) {
    result = MRB_DUMP_GENERAL_FAILURE;
  }

lv_exit:
  mrb_str_resize(mrb, syms_obj, 0);
  return result;
}

static int
read_binary_header(const uint8_t *bin, size_t bufsize, size_t *bin_size, uint16_t *crc, uint8_t *flags)
{
  const struct rite_binary_header *header = (const struct rite_binary_header *)bin;

  if (bufsize < sizeof(struct rite_binary_header)) {
    return MRB_DUMP_READ_FAULT;
  }

  if (memcmp(header->binary_ident, RITE_BINARY_IDENT, sizeof(header->binary_ident)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;
  }

  if (memcmp(header->binary_version, RITE_BINARY_FORMAT_VER, sizeof(header->binary_version)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;
  }

  if (crc) {
    *crc = bin_to_uint16(header->binary_crc);
  }
  *bin_size = (size_t)bin_to_uint32(header->binary_size);

  if (bufsize < *bin_size) {
    return MRB_DUMP_READ_FAULT;
  }

  return MRB_DUMP_OK;
}

static mrb_irep*
read_irep(mrb_state *mrb, const uint8_t *bin, size_t bufsize, uint8_t flags)
{
  int result;
  struct RData *irep_obj = NULL;
  mrb_irep *irep = NULL;
  const struct rite_section_header *section_header;
  uint16_t crc;
  size_t bin_size = 0;
  size_t n;

  if ((mrb == NULL) || (bin == NULL)) {
    return NULL;
  }

  result = read_binary_header(bin, bufsize, &bin_size, &crc, &flags);
  if (result != MRB_DUMP_OK) {
    return NULL;
  }

  n = offset_crc_body();
  if (crc != calc_crc_16_ccitt(bin + n, bin_size - n, 0)) {
    return NULL;
  }

  irep_obj = mrb_data_object_alloc(mrb, mrb->object_class, NULL, &load_tempirep_type);

  bin += sizeof(struct rite_binary_header);
  do {
    section_header = (const struct rite_section_header *)bin;
    if (memcmp(section_header->section_ident, RITE_SECTION_IREP_IDENT, sizeof(section_header->section_ident)) == 0) {
      irep = read_section_irep(mrb, bin, flags);
      if (!irep) return NULL;
      irep_obj->data = irep;
    }
    else if (memcmp(section_header->section_ident, RITE_SECTION_DEBUG_IDENT, sizeof(section_header->section_ident)) == 0) {
      if (!irep) return NULL;   /* corrupted data */
      result = read_section_debug(mrb, bin, irep, flags);
      if (result < MRB_DUMP_OK) {
        return NULL;
      }
    }
    else if (memcmp(section_header->section_ident, RITE_SECTION_LV_IDENT, sizeof(section_header->section_ident)) == 0) {
      if (!irep) return NULL;
      result = read_section_lv(mrb, bin, irep, flags);
      if (result < MRB_DUMP_OK) {
        return NULL;
      }
    }
    bin += bin_to_uint32(section_header->section_size);
  } while (memcmp(section_header->section_ident, RITE_BINARY_EOF, sizeof(section_header->section_ident)) != 0);

  irep_obj->data = NULL;

  return irep;
}

mrb_irep*
mrb_read_irep(mrb_state *mrb, const uint8_t *bin)
{
#if defined(MRB_USE_LINK_TIME_RO_DATA_P) || defined(MRB_USE_CUSTOM_RO_DATA_P)
  uint8_t flags = mrb_ro_data_p((char*)bin) ? FLAG_SRC_STATIC : FLAG_SRC_MALLOC;
#else
  uint8_t flags = FLAG_SRC_STATIC;
#endif

  return read_irep(mrb, bin, (size_t)-1, flags);
}

MRB_API mrb_irep*
mrb_read_irep_buf(mrb_state *mrb, const void *buf, size_t bufsize)
{
  return read_irep(mrb, (const uint8_t *)buf, bufsize, FLAG_SRC_MALLOC);
}

void mrb_exc_set(mrb_state *mrb, mrb_value exc);

static void
irep_error(mrb_state *mrb)
{
  mrb_exc_set(mrb, mrb_exc_new_str_lit(mrb, E_SCRIPT_ERROR, "irep load error"));
}

void mrb_codedump_all(mrb_state*, struct RProc*);

static mrb_value
load_irep(mrb_state *mrb, mrb_irep *irep, mrbc_context *c)
{
  struct RProc *proc;

  if (!irep) {
    irep_error(mrb);
    return mrb_nil_value();
  }
  proc = mrb_proc_new(mrb, irep);
  proc->c = NULL;
  mrb_irep_decref(mrb, irep);
  if (c && c->dump_result) mrb_codedump_all(mrb, proc);
  if (c && c->no_exec) return mrb_obj_value(proc);
  return mrb_top_run(mrb, proc, mrb_top_self(mrb), 0);
}

MRB_API mrb_value
mrb_load_irep_cxt(mrb_state *mrb, const uint8_t *bin, mrbc_context *c)
{
  struct RData *irep_obj = mrb_data_object_alloc(mrb, mrb->object_class, NULL, &load_tempirep_type);
  mrb_irep *irep = mrb_read_irep(mrb, bin);
  mrb_value ret;

  irep_obj->data = irep;
  mrb_irep_incref(mrb, irep);
  ret = load_irep(mrb, irep, c);
  irep_obj->data = NULL;
  mrb_irep_decref(mrb, irep);
  return ret;
}

MRB_API mrb_value
mrb_load_irep_buf_cxt(mrb_state *mrb, const void *buf, size_t bufsize, mrbc_context *c)
{
  return load_irep(mrb, mrb_read_irep_buf(mrb, buf, bufsize), c);
}

MRB_API mrb_value
mrb_load_irep(mrb_state *mrb, const uint8_t *bin)
{
  return mrb_load_irep_cxt(mrb, bin, NULL);
}

MRB_API mrb_value
mrb_load_irep_buf(mrb_state *mrb, const void *buf, size_t bufsize)
{
  return mrb_load_irep_buf_cxt(mrb, buf, bufsize, NULL);
}

#ifndef MRB_DISABLE_STDIO

mrb_irep*
mrb_read_irep_file(mrb_state *mrb, FILE* fp)
{
  mrb_irep *irep = NULL;
  uint8_t *buf;
  const size_t header_size = sizeof(struct rite_binary_header);
  size_t buf_size = 0;
  uint8_t flags = 0;
  int result;

  if ((mrb == NULL) || (fp == NULL)) {
    return NULL;
  }

  buf = (uint8_t*)mrb_malloc(mrb, header_size);
  if (fread(buf, header_size, 1, fp) == 0) {
    goto irep_exit;
  }
  result = read_binary_header(buf, (size_t)-1, &buf_size, NULL, &flags);
  if (result != MRB_DUMP_OK || buf_size <= header_size) {
    goto irep_exit;
  }

  buf = (uint8_t*)mrb_realloc(mrb, buf, buf_size);
  if (fread(buf+header_size, buf_size-header_size, 1, fp) == 0) {
    goto irep_exit;
  }
  irep = read_irep(mrb, buf, (size_t)-1, FLAG_SRC_MALLOC);

irep_exit:
  mrb_free(mrb, buf);
  return irep;
}

MRB_API mrb_value
mrb_load_irep_file_cxt(mrb_state *mrb, FILE* fp, mrbc_context *c)
{
  return load_irep(mrb, mrb_read_irep_file(mrb, fp), c);
}

MRB_API mrb_value
mrb_load_irep_file(mrb_state *mrb, FILE* fp)
{
  return mrb_load_irep_file_cxt(mrb, fp, NULL);
}
#endif /* MRB_DISABLE_STDIO */
/*
** numeric.c - Numeric, Integer, Float, Fixnum class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRB_WITHOUT_FLOAT
#include <float.h>
#include <math.h>
#endif
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include <mruby/class.h>

#ifndef MRB_WITHOUT_FLOAT
#ifdef MRB_USE_FLOAT
#define trunc(f) truncf(f)
#define floor(f) floorf(f)
#define ceil(f) ceilf(f)
#define fmod(x,y) fmodf(x,y)
#define FLO_TO_STR_PREC 8
#else
#define FLO_TO_STR_PREC 16
#endif
#endif

#ifndef MRB_WITHOUT_FLOAT
MRB_API mrb_float
mrb_to_flo(mrb_state *mrb, mrb_value val)
{
  switch (mrb_type(val)) {
  case MRB_TT_FIXNUM:
    return (mrb_float)mrb_fixnum(val);
  case MRB_TT_FLOAT:
    break;
  default:
    mrb_raise(mrb, E_TYPE_ERROR, "non float value");
  }
  return mrb_float(val);
}

MRB_API mrb_value
mrb_int_value(mrb_state *mrb, mrb_float f)
{
  if (FIXABLE_FLOAT(f)) {
    return mrb_fixnum_value((mrb_int)f);
  }
  return mrb_float_value(mrb, f);
}
#endif

/*
 * call-seq:
 *
 *  num ** other  ->  num
 *
 * Raises <code>num</code> the <code>other</code> power.
 *
 *    2.0**3      #=> 8.0
 */
static mrb_value
integral_pow(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
#ifndef MRB_WITHOUT_FLOAT
  mrb_float d;
#endif

  if (mrb_fixnum_p(x) && mrb_fixnum_p(y)) {
    /* try ipow() */
    mrb_int base = mrb_fixnum(x);
    mrb_int exp = mrb_fixnum(y);
    mrb_int result = 1;

    if (exp < 0)
#ifdef MRB_WITHOUT_FLOAT
      return mrb_fixnum_value(0);
#else
      goto float_pow;
#endif
    for (;;) {
      if (exp & 1) {
        if (mrb_int_mul_overflow(result, base, &result)) {
#ifndef MRB_WITHOUT_FLOAT
          goto float_pow;
#endif
        }
      }
      exp >>= 1;
      if (exp == 0) break;
      if (mrb_int_mul_overflow(base, base, &base)) {
#ifndef MRB_WITHOUT_FLOAT
        goto float_pow;
#endif
      }
    }
    return mrb_fixnum_value(result);
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
 float_pow:
  d = pow(mrb_to_flo(mrb, x), mrb_to_flo(mrb, y));
  return mrb_float_value(mrb, d);
#endif
}

static mrb_value
integral_idiv(mrb_state *mrb, mrb_value x)
{
#ifdef MRB_WITHOUT_FLOAT
  mrb_value y = mrb_get_arg1(mrb);

  if (!mrb_fixnum_p(y)) {
    mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
  }
  return mrb_fixnum_value(mrb_fixnum(x) / mrb_fixnum(y));
#else
  mrb_float y;

  mrb_get_args(mrb, "f", &y);
  return mrb_int_value(mrb, mrb_to_flo(mrb, x) / y);
#endif
}

/* 15.2.8.3.4  */
/* 15.2.9.3.4  */
/*
 * call-seq:
 *   num / other  ->  num
 *
 * Performs division: the class of the resulting object depends on
 * the class of <code>num</code> and on the magnitude of the
 * result.
 */

/* 15.2.9.3.19(x) */
/*
 *  call-seq:
 *     num.quo(numeric)  ->  real
 *
 *  Returns most exact division.
 */

static mrb_value
integral_div(mrb_state *mrb, mrb_value x)
{
#ifdef MRB_WITHOUT_FLOAT
  mrb_value y = mrb_get_arg1(mrb);

  if (!mrb_fixnum_p(y)) {
    mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
  }
  return mrb_fixnum_value(mrb_fixnum(x) / mrb_fixnum(y));
#else
  mrb_float y;

  mrb_get_args(mrb, "f", &y);
  return mrb_float_value(mrb, mrb_to_flo(mrb, x) / y);
#endif
}

static mrb_value
integral_coerce_step_counter(mrb_state *mrb, mrb_value self)
{
  mrb_value num, step;

  mrb_get_args(mrb, "oo", &num, &step);

#ifndef MRB_WITHOUT_FLOAT
  if (mrb_float_p(self) || mrb_float_p(num) || mrb_float_p(step)) {
    return mrb_Float(mrb, self);
  }
#endif

  return self;
}

#ifndef MRB_WITHOUT_FLOAT
/********************************************************************
 *
 * Document-class: Float
 *
 *  <code>Float</code> objects represent inexact real numbers using
 *  the native architecture's double-precision floating point
 *  representation.
 */

/* 15.2.9.3.16(x) */
/*
 *  call-seq:
 *     flt.to_s  ->  string
 *
 *  Returns a string containing a representation of self. As well as a
 *  fixed or exponential form of the number, the call may return
 *  "<code>NaN</code>", "<code>Infinity</code>", and
 *  "<code>-Infinity</code>".
 */

static mrb_value
flo_to_s(mrb_state *mrb, mrb_value flt)
{
  mrb_float f = mrb_float(flt);
  mrb_value str;

  if (isinf(f)) {
    str = f < 0 ? mrb_str_new_lit(mrb, "-Infinity")
                : mrb_str_new_lit(mrb, "Infinity");
    goto exit;
  }
  else if (isnan(f)) {
    str = mrb_str_new_lit(mrb, "NaN");
    goto exit;
  }
  else {
    char fmt[] = "%." MRB_STRINGIZE(FLO_TO_STR_PREC) "g";
    mrb_int len;
    char *begp, *p, *endp;

    str = mrb_float_to_str(mrb, flt, fmt);

    insert_dot_zero:
    begp = RSTRING_PTR(str);
    len = RSTRING_LEN(str);
    for (p = begp, endp = p + len; p < endp; ++p) {
      if (*p == '.') {
        goto exit;
      }
      else if (*p == 'e') {
        ptrdiff_t e_pos = p - begp;
        mrb_str_cat(mrb, str, ".0", 2);
        p = RSTRING_PTR(str) + e_pos;
        memmove(p + 2, p, len - e_pos);
        memcpy(p, ".0", 2);
        goto exit;
      }
    }

    if (FLO_TO_STR_PREC + (begp[0] == '-') <= len) {
      --fmt[sizeof(fmt) - 3];  /* %.16g(%.8g) -> %.15g(%.7g) */
      str = mrb_float_to_str(mrb, flt, fmt);
      goto insert_dot_zero;
    }

    goto exit;
  }

  exit:
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

/* 15.2.9.3.2  */
/*
 * call-seq:
 *   float - other  ->  float
 *
 * Returns a new float which is the difference of <code>float</code>
 * and <code>other</code>.
 */

static mrb_value
flo_minus(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  return mrb_float_value(mrb, mrb_float(x) - mrb_to_flo(mrb, y));
}

/* 15.2.9.3.3  */
/*
 * call-seq:
 *   float * other  ->  float
 *
 * Returns a new float which is the product of <code>float</code>
 * and <code>other</code>.
 */

static mrb_value
flo_mul(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  return mrb_float_value(mrb, mrb_float(x) * mrb_to_flo(mrb, y));
}

static void
flodivmod(mrb_state *mrb, double x, double y, mrb_float *divp, mrb_float *modp)
{
  double div, mod;

  if (isnan(y)) {
    /* y is NaN so all results are NaN */
    div = mod = y;
    goto exit;
  }
  if (y == 0.0) {
    if (x == 0) div = NAN;
    else if (x > 0.0) div = INFINITY;
    else div = -INFINITY;       /* x < 0.0 */
    mod = NAN;
    goto exit;
  }
  if ((x == 0.0) || (isinf(y) && !isinf(x))) {
    mod = x;
  }
  else {
    mod = fmod(x, y);
  }
  if (isinf(x) && !isinf(y)) {
    div = x;
  }
  else {
    div = (x - mod) / y;
    if (modp && divp) div = round(div);
  }
  if (div == 0) div = 0.0;
  if (mod == 0) mod = 0.0;
  if (y*mod < 0) {
    mod += y;
    div -= 1.0;
  }
 exit:
  if (modp) *modp = mod;
  if (divp) *divp = div;
}

/* 15.2.9.3.5  */
/*
 *  call-seq:
 *     flt % other        ->  float
 *     flt.modulo(other)  ->  float
 *
 *  Return the modulo after division of <code>flt</code> by <code>other</code>.
 *
 *     6543.21.modulo(137)      #=> 104.21
 *     6543.21.modulo(137.24)   #=> 92.9299999999996
 */

static mrb_value
flo_mod(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  mrb_float mod;

  flodivmod(mrb, mrb_float(x), mrb_to_flo(mrb, y), 0, &mod);
  return mrb_float_value(mrb, mod);
}
#endif

/* 15.2.8.3.16 */
/*
 *  call-seq:
 *     num.eql?(numeric)  ->  true or false
 *
 *  Returns <code>true</code> if <i>num</i> and <i>numeric</i> are the
 *  same type and have equal values.
 *
 *     1 == 1.0          #=> true
 *     1.eql?(1.0)       #=> false
 *     (1.0).eql?(1.0)   #=> true
 */
static mrb_value
fix_eql(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  if (!mrb_fixnum_p(y)) return mrb_false_value();
  return mrb_bool_value(mrb_fixnum(x) == mrb_fixnum(y));
}

#ifndef MRB_WITHOUT_FLOAT
static mrb_value
flo_eql(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  if (!mrb_float_p(y)) return mrb_false_value();
  return mrb_bool_value(mrb_float(x) == mrb_float(y));
}

/* 15.2.9.3.7  */
/*
 *  call-seq:
 *     flt == obj  ->  true or false
 *
 *  Returns <code>true</code> only if <i>obj</i> has the same value
 *  as <i>flt</i>. Contrast this with <code>Float#eql?</code>, which
 *  requires <i>obj</i> to be a <code>Float</code>.
 *
 *     1.0 == 1   #=> true
 *
 */

static mrb_value
flo_eq(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  switch (mrb_type(y)) {
  case MRB_TT_FIXNUM:
    return mrb_bool_value(mrb_float(x) == (mrb_float)mrb_fixnum(y));
  case MRB_TT_FLOAT:
    return mrb_bool_value(mrb_float(x) == mrb_float(y));
  default:
    return mrb_false_value();
  }
}

static int64_t
value_int64(mrb_state *mrb, mrb_value x)
{
  switch (mrb_type(x)) {
  case MRB_TT_FIXNUM:
    return (int64_t)mrb_fixnum(x);
    break;
  case MRB_TT_FLOAT:
    return (int64_t)mrb_float(x);
  default:
    mrb_raise(mrb, E_TYPE_ERROR, "cannot convert to Integer");
    break;
  }
  /* not reached */
  return 0;
}

static mrb_value
int64_value(mrb_state *mrb, int64_t v)
{
  if (TYPED_FIXABLE(v,int64_t)) {
    return mrb_fixnum_value((mrb_int)v);
  }
  return mrb_float_value(mrb, (mrb_float)v);
}

static mrb_value
flo_rev(mrb_state *mrb, mrb_value x)
{
  int64_t v1;
  v1 = (int64_t)mrb_float(x);
  return int64_value(mrb, ~v1);
}

static mrb_value
flo_and(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  int64_t v1, v2;

  v1 = (int64_t)mrb_float(x);
  v2 = value_int64(mrb, y);
  return int64_value(mrb, v1 & v2);
}

static mrb_value
flo_or(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  int64_t v1, v2;

  v1 = (int64_t)mrb_float(x);
  v2 = value_int64(mrb, y);
  return int64_value(mrb, v1 | v2);
}

static mrb_value
flo_xor(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  int64_t v1, v2;

  v1 = (int64_t)mrb_float(x);
  v2 = value_int64(mrb, y);
  return int64_value(mrb, v1 ^ v2);
}

static mrb_value
flo_shift(mrb_state *mrb, mrb_value x, mrb_int width)
{
  mrb_float val;

  if (width == 0) {
    return x;
  }
  val = mrb_float(x);
  if (width < 0) {
    while (width++) {
      val /= 2;
      if (val < 1.0) {
        val = 0;
        break;
      }
    }
#if defined(_ISOC99_SOURCE)
    val = trunc(val);
#else
    if (val > 0){
        val = floor(val);
    } else {
        val = ceil(val);
    }
#endif
    if (val == 0 && mrb_float(x) < 0) {
      return mrb_fixnum_value(-1);
    }
  }
  else {
    while (width--) {
      val *= 2;
    }
  }
  return mrb_int_value(mrb, val);
}

static mrb_value
flo_rshift(mrb_state *mrb, mrb_value x)
{
  mrb_int width;

  mrb_get_args(mrb, "i", &width);
  return flo_shift(mrb, x, -width);
}

static mrb_value
flo_lshift(mrb_state *mrb, mrb_value x)
{
  mrb_int width;

  mrb_get_args(mrb, "i", &width);
  return flo_shift(mrb, x, width);
}

/* 15.2.9.3.13 */
/*
 * call-seq:
 *   flt.to_f  ->  self
 *
 * As <code>flt</code> is already a float, returns +self+.
 */

static mrb_value
flo_to_f(mrb_state *mrb, mrb_value num)
{
  return num;
}

/* 15.2.9.3.11 */
/*
 *  call-seq:
 *     flt.infinite?  ->  nil, -1, +1
 *
 *  Returns <code>nil</code>, -1, or +1 depending on whether <i>flt</i>
 *  is finite, -infinity, or +infinity.
 *
 *     (0.0).infinite?        #=> nil
 *     (-1.0/0.0).infinite?   #=> -1
 *     (+1.0/0.0).infinite?   #=> 1
 */

static mrb_value
flo_infinite_p(mrb_state *mrb, mrb_value num)
{
  mrb_float value = mrb_float(num);

  if (isinf(value)) {
    return mrb_fixnum_value(value < 0 ? -1 : 1);
  }
  return mrb_nil_value();
}

/* 15.2.9.3.9  */
/*
 *  call-seq:
 *     flt.finite?  ->  true or false
 *
 *  Returns <code>true</code> if <i>flt</i> is a valid IEEE floating
 *  point number (it is not infinite, and <code>nan?</code> is
 *  <code>false</code>).
 *
 */

static mrb_value
flo_finite_p(mrb_state *mrb, mrb_value num)
{
  return mrb_bool_value(isfinite(mrb_float(num)));
}

void
mrb_check_num_exact(mrb_state *mrb, mrb_float num)
{
  if (isinf(num)) {
    mrb_raise(mrb, E_FLOATDOMAIN_ERROR, num < 0 ? "-Infinity" : "Infinity");
  }
  if (isnan(num)) {
    mrb_raise(mrb, E_FLOATDOMAIN_ERROR, "NaN");
  }
}

/* 15.2.9.3.10 */
/*
 *  call-seq:
 *     flt.floor  ->  integer
 *
 *  Returns the largest integer less than or equal to <i>flt</i>.
 *
 *     1.2.floor      #=> 1
 *     2.0.floor      #=> 2
 *     (-1.2).floor   #=> -2
 *     (-2.0).floor   #=> -2
 */

static mrb_value
flo_floor(mrb_state *mrb, mrb_value num)
{
  mrb_float f = floor(mrb_float(num));

  mrb_check_num_exact(mrb, f);
  return mrb_int_value(mrb, f);
}

/* 15.2.9.3.8  */
/*
 *  call-seq:
 *     flt.ceil  ->  integer
 *
 *  Returns the smallest <code>Integer</code> greater than or equal to
 *  <i>flt</i>.
 *
 *     1.2.ceil      #=> 2
 *     2.0.ceil      #=> 2
 *     (-1.2).ceil   #=> -1
 *     (-2.0).ceil   #=> -2
 */

static mrb_value
flo_ceil(mrb_state *mrb, mrb_value num)
{
  mrb_float f = ceil(mrb_float(num));

  mrb_check_num_exact(mrb, f);
  return mrb_int_value(mrb, f);
}

/* 15.2.9.3.12 */
/*
 *  call-seq:
 *     flt.round([ndigits])  ->  integer or float
 *
 *  Rounds <i>flt</i> to a given precision in decimal digits (default 0 digits).
 *  Precision may be negative.  Returns a floating point number when ndigits
 *  is more than zero.
 *
 *     1.4.round      #=> 1
 *     1.5.round      #=> 2
 *     1.6.round      #=> 2
 *     (-1.5).round   #=> -2
 *
 *     1.234567.round(2)  #=> 1.23
 *     1.234567.round(3)  #=> 1.235
 *     1.234567.round(4)  #=> 1.2346
 *     1.234567.round(5)  #=> 1.23457
 *
 *     34567.89.round(-5) #=> 0
 *     34567.89.round(-4) #=> 30000
 *     34567.89.round(-3) #=> 35000
 *     34567.89.round(-2) #=> 34600
 *     34567.89.round(-1) #=> 34570
 *     34567.89.round(0)  #=> 34568
 *     34567.89.round(1)  #=> 34567.9
 *     34567.89.round(2)  #=> 34567.89
 *     34567.89.round(3)  #=> 34567.89
 *
 */

static mrb_value
flo_round(mrb_state *mrb, mrb_value num)
{
  double number, f;
  mrb_int ndigits = 0;
  mrb_int i;

  mrb_get_args(mrb, "|i", &ndigits);
  number = mrb_float(num);

  if (0 < ndigits && (isinf(number) || isnan(number))) {
    return num;
  }
  mrb_check_num_exact(mrb, number);

  f = 1.0;
  i = ndigits >= 0 ? ndigits : -ndigits;
  if (ndigits > DBL_DIG+2) return num;
  while  (--i >= 0)
    f = f*10.0;

  if (isinf(f)) {
    if (ndigits < 0) number = 0;
  }
  else {
    double d;

    if (ndigits < 0) number /= f;
    else number *= f;

    /* home-made inline implementation of round(3) */
    if (number > 0.0) {
      d = floor(number);
      number = d + (number - d >= 0.5);
    }
    else if (number < 0.0) {
      d = ceil(number);
      number = d - (d - number >= 0.5);
    }

    if (ndigits < 0) number *= f;
    else number /= f;
  }

  if (ndigits > 0) {
    if (!isfinite(number)) return num;
    return mrb_float_value(mrb, number);
  }
  return mrb_int_value(mrb, number);
}

/* 15.2.9.3.14 */
/* 15.2.9.3.15 */
/*
 *  call-seq:
 *     flt.to_i      ->  integer
 *     flt.truncate  ->  integer
 *
 *  Returns <i>flt</i> truncated to an <code>Integer</code>.
 */

static mrb_value
flo_truncate(mrb_state *mrb, mrb_value num)
{
  mrb_float f = mrb_float(num);

  if (f > 0.0) f = floor(f);
  if (f < 0.0) f = ceil(f);

  mrb_check_num_exact(mrb, f);
  return mrb_int_value(mrb, f);
}

static mrb_value
flo_nan_p(mrb_state *mrb, mrb_value num)
{
  return mrb_bool_value(isnan(mrb_float(num)));
}
#endif

/*
 * Document-class: Integer
 *
 *  <code>Integer</code> is the basis for the two concrete classes that
 *  hold whole numbers, <code>Bignum</code> and <code>Fixnum</code>.
 *
 */


/*
 *  call-seq:
 *     int.to_i      ->  integer
 *
 *  As <i>int</i> is already an <code>Integer</code>, all these
 *  methods simply return the receiver.
 */

static mrb_value
int_to_i(mrb_state *mrb, mrb_value num)
{
  return num;
}

static mrb_value
fixnum_mul(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;

  a = mrb_fixnum(x);
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    if (a == 0) return x;
    b = mrb_fixnum(y);
    if (mrb_int_mul_overflow(a, b, &c)) {
#ifndef MRB_WITHOUT_FLOAT
      return mrb_float_value(mrb, (mrb_float)a * (mrb_float)b);
#endif
    }
    return mrb_fixnum_value(c);
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
  return mrb_float_value(mrb, (mrb_float)a * mrb_to_flo(mrb, y));
#endif
}

MRB_API mrb_value
mrb_num_mul(mrb_state *mrb, mrb_value x, mrb_value y)
{
  if (mrb_fixnum_p(x)) {
    return fixnum_mul(mrb, x, y);
  }
#ifndef MRB_WITHOUT_FLOAT
  if (mrb_float_p(x)) {
    return mrb_float_value(mrb, mrb_float(x) * mrb_to_flo(mrb, y));
  }
#endif
  mrb_raise(mrb, E_TYPE_ERROR, "no number multiply");
  return mrb_nil_value();       /* not reached */
}

/* 15.2.8.3.3  */
/*
 * call-seq:
 *   fix * numeric  ->  numeric_result
 *
 * Performs multiplication: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */

static mrb_value
fix_mul(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  return fixnum_mul(mrb, x, y);
}

static void
fixdivmod(mrb_state *mrb, mrb_int x, mrb_int y, mrb_int *divp, mrb_int *modp)
{
  mrb_int div, mod;

  /* TODO: add mrb_assert(y != 0) to make sure */

  if (y < 0) {
    if (x < 0)
      div = -x / -y;
    else
      div = - (x / -y);
  }
  else {
    if (x < 0)
      div = - (-x / y);
    else
      div = x / y;
  }
  mod = x - div*y;
  if ((mod < 0 && y > 0) || (mod > 0 && y < 0)) {
    mod += y;
    div -= 1;
  }
  if (divp) *divp = div;
  if (modp) *modp = mod;
}

/* 15.2.8.3.5  */
/*
 *  call-seq:
 *    fix % other        ->  real
 *    fix.modulo(other)  ->  real
 *
 *  Returns <code>fix</code> modulo <code>other</code>.
 *  See <code>numeric.divmod</code> for more information.
 */

static mrb_value
fix_mod(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  mrb_int a, b;

  a = mrb_fixnum(x);
   if (mrb_fixnum_p(y) && a != MRB_INT_MIN && (b=mrb_fixnum(y)) != MRB_INT_MIN) {
    mrb_int mod;

    if (b == 0) {
#ifdef MRB_WITHOUT_FLOAT
      /* ZeroDivisionError */
      return mrb_fixnum_value(0);
#else
      if (a > 0) return mrb_float_value(mrb, INFINITY);
      if (a < 0) return mrb_float_value(mrb, INFINITY);
      return mrb_float_value(mrb, NAN);
#endif
    }
    fixdivmod(mrb, a, b, NULL, &mod);
    return mrb_fixnum_value(mod);
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
  else {
    mrb_float mod;

    flodivmod(mrb, (mrb_float)a, mrb_to_flo(mrb, y), NULL, &mod);
    return mrb_float_value(mrb, mod);
  }
#endif
}

/*
 *  call-seq:
 *     fix.divmod(numeric)  ->  array
 *
 *  See <code>Numeric#divmod</code>.
 */
static mrb_value
fix_divmod(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  if (mrb_fixnum_p(y)) {
    mrb_int div, mod;

    if (mrb_fixnum(y) == 0) {
#ifdef MRB_WITHOUT_FLOAT
      return mrb_assoc_new(mrb, mrb_fixnum_value(0), mrb_fixnum_value(0));
#else
      return mrb_assoc_new(mrb, ((mrb_fixnum(x) == 0) ?
                                 mrb_float_value(mrb, NAN):
                                 mrb_float_value(mrb, INFINITY)),
                           mrb_float_value(mrb, NAN));
#endif
    }
    fixdivmod(mrb, mrb_fixnum(x), mrb_fixnum(y), &div, &mod);
    return mrb_assoc_new(mrb, mrb_fixnum_value(div), mrb_fixnum_value(mod));
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
  else {
    mrb_float div, mod;
    mrb_value a, b;

    flodivmod(mrb, (mrb_float)mrb_fixnum(x), mrb_to_flo(mrb, y), &div, &mod);
    a = mrb_int_value(mrb, div);
    b = mrb_float_value(mrb, mod);
    return mrb_assoc_new(mrb, a, b);
  }
#endif
}

#ifndef MRB_WITHOUT_FLOAT
static mrb_value
flo_divmod(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);
  mrb_float div, mod;
  mrb_value a, b;

  flodivmod(mrb, mrb_float(x), mrb_to_flo(mrb, y), &div, &mod);
  a = mrb_int_value(mrb, div);
  b = mrb_float_value(mrb, mod);
  return mrb_assoc_new(mrb, a, b);
}
#endif

/* 15.2.8.3.7  */
/*
 * call-seq:
 *   fix == other  ->  true or false
 *
 * Return <code>true</code> if <code>fix</code> equals <code>other</code>
 * numerically.
 *
 *   1 == 2      #=> false
 *   1 == 1.0    #=> true
 */

static mrb_value
fix_equal(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  switch (mrb_type(y)) {
  case MRB_TT_FIXNUM:
    return mrb_bool_value(mrb_fixnum(x) == mrb_fixnum(y));
#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
    return mrb_bool_value((mrb_float)mrb_fixnum(x) == mrb_float(y));
#endif
  default:
    return mrb_false_value();
  }
}

/* 15.2.8.3.8  */
/*
 * call-seq:
 *   ~fix  ->  integer
 *
 * One's complement: returns a number where each bit is flipped.
 *   ex.0---00001 (1)-> 1---11110 (-2)
 *   ex.0---00010 (2)-> 1---11101 (-3)
 *   ex.0---00100 (4)-> 1---11011 (-5)
 */

static mrb_value
fix_rev(mrb_state *mrb, mrb_value num)
{
  mrb_int val = mrb_fixnum(num);

  return mrb_fixnum_value(~val);
}

#ifdef MRB_WITHOUT_FLOAT
#define bit_op(x,y,op1,op2) do {\
  return mrb_fixnum_value(mrb_fixnum(x) op2 mrb_fixnum(y));\
} while(0)
#else
static mrb_value flo_and(mrb_state *mrb, mrb_value x);
static mrb_value flo_or(mrb_state *mrb, mrb_value x);
static mrb_value flo_xor(mrb_state *mrb, mrb_value x);
#define bit_op(x,y,op1,op2) do {\
  if (mrb_fixnum_p(y)) return mrb_fixnum_value(mrb_fixnum(x) op2 mrb_fixnum(y));\
  return flo_ ## op1(mrb, mrb_float_value(mrb, (mrb_float)mrb_fixnum(x)));\
} while(0)
#endif

/* 15.2.8.3.9  */
/*
 * call-seq:
 *   fix & integer  ->  integer_result
 *
 * Bitwise AND.
 */

static mrb_value
fix_and(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  bit_op(x, y, and, &);
}

/* 15.2.8.3.10 */
/*
 * call-seq:
 *   fix | integer  ->  integer_result
 *
 * Bitwise OR.
 */

static mrb_value
fix_or(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  bit_op(x, y, or, |);
}

/* 15.2.8.3.11 */
/*
 * call-seq:
 *   fix ^ integer  ->  integer_result
 *
 * Bitwise EXCLUSIVE OR.
 */

static mrb_value
fix_xor(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  bit_op(x, y, or, ^);
}

#define NUMERIC_SHIFT_WIDTH_MAX (MRB_INT_BIT-1)

static mrb_value
lshift(mrb_state *mrb, mrb_int val, mrb_int width)
{
  if (width < 0) {              /* mrb_int overflow */
#ifdef MRB_WITHOUT_FLOAT
    return mrb_fixnum_value(0);
#else
    return mrb_float_value(mrb, INFINITY);
#endif
  }
  if (val > 0) {
    if ((width > NUMERIC_SHIFT_WIDTH_MAX) ||
        (val   > (MRB_INT_MAX >> width))) {
#ifdef MRB_WITHOUT_FLOAT
      return mrb_fixnum_value(-1);
#else
      goto bit_overflow;
#endif
    }
    return mrb_fixnum_value(val << width);
  }
  else {
    if ((width > NUMERIC_SHIFT_WIDTH_MAX) ||
        (val   <= (MRB_INT_MIN >> width))) {
#ifdef MRB_WITHOUT_FLOAT
      return mrb_fixnum_value(0);
#else
      goto bit_overflow;
#endif
    }
    return mrb_fixnum_value(val * ((mrb_int)1 << width));
  }

#ifndef MRB_WITHOUT_FLOAT
bit_overflow:
  {
    mrb_float f = (mrb_float)val;
    while (width--) {
      f *= 2;
    }
    return mrb_float_value(mrb, f);
  }
#endif
}

static mrb_value
rshift(mrb_int val, mrb_int width)
{
  if (width < 0) {              /* mrb_int overflow */
    return mrb_fixnum_value(0);
  }
  if (width >= NUMERIC_SHIFT_WIDTH_MAX) {
    if (val < 0) {
      return mrb_fixnum_value(-1);
    }
    return mrb_fixnum_value(0);
  }
  return mrb_fixnum_value(val >> width);
}

/* 15.2.8.3.12 */
/*
 * call-seq:
 *   fix << count  ->  integer or float
 *
 * Shifts _fix_ left _count_ positions (right if _count_ is negative).
 */

static mrb_value
fix_lshift(mrb_state *mrb, mrb_value x)
{
  mrb_int width, val;

  mrb_get_args(mrb, "i", &width);
  if (width == 0) {
    return x;
  }
  val = mrb_fixnum(x);
  if (val == 0) return x;
  if (width < 0) {
    return rshift(val, -width);
  }
  return lshift(mrb, val, width);
}

/* 15.2.8.3.13 */
/*
 * call-seq:
 *   fix >> count  ->  integer or float
 *
 * Shifts _fix_ right _count_ positions (left if _count_ is negative).
 */

static mrb_value
fix_rshift(mrb_state *mrb, mrb_value x)
{
  mrb_int width, val;

  mrb_get_args(mrb, "i", &width);
  if (width == 0) {
    return x;
  }
  val = mrb_fixnum(x);
  if (val == 0) return x;
  if (width < 0) {
    return lshift(mrb, val, -width);
  }
  return rshift(val, width);
}

/* 15.2.8.3.23 */
/*
 *  call-seq:
 *     fix.to_f  ->  float
 *
 *  Converts <i>fix</i> to a <code>Float</code>.
 *
 */

#ifndef MRB_WITHOUT_FLOAT
static mrb_value
fix_to_f(mrb_state *mrb, mrb_value num)
{
  return mrb_float_value(mrb, (mrb_float)mrb_fixnum(num));
}

/*
 *  Document-class: FloatDomainError
 *
 *  Raised when attempting to convert special float values
 *  (in particular infinite or NaN)
 *  to numerical classes which don't support them.
 *
 *     Float::INFINITY.to_i
 *
 *  <em>raises the exception:</em>
 *
 *     FloatDomainError: Infinity
 */
/* ------------------------------------------------------------------------*/
MRB_API mrb_value
mrb_flo_to_fixnum(mrb_state *mrb, mrb_value x)
{
  mrb_int z = 0;

  if (!mrb_float_p(x)) {
    mrb_raise(mrb, E_TYPE_ERROR, "non float value");
    z = 0; /* not reached. just suppress warnings. */
  }
  else {
    mrb_float d = mrb_float(x);

    mrb_check_num_exact(mrb, d);
    if (FIXABLE_FLOAT(d)) {
      z = (mrb_int)d;
    }
    else {
      mrb_raisef(mrb, E_RANGE_ERROR, "number (%v) too big for integer", x);
    }
  }
  return mrb_fixnum_value(z);
}
#endif

static mrb_value
fixnum_plus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;

  a = mrb_fixnum(x);
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    if (a == 0) return y;
    b = mrb_fixnum(y);
    if (mrb_int_add_overflow(a, b, &c)) {
#ifndef MRB_WITHOUT_FLOAT
      return mrb_float_value(mrb, (mrb_float)a + (mrb_float)b);
#endif
    }
    return mrb_fixnum_value(c);
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
  return mrb_float_value(mrb, (mrb_float)a + mrb_to_flo(mrb, y));
#endif
}

MRB_API mrb_value
mrb_num_plus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  if (mrb_fixnum_p(x)) {
    return fixnum_plus(mrb, x, y);
  }
#ifndef MRB_WITHOUT_FLOAT
  if (mrb_float_p(x)) {
    return mrb_float_value(mrb, mrb_float(x) + mrb_to_flo(mrb, y));
  }
#endif
  mrb_raise(mrb, E_TYPE_ERROR, "no number addition");
  return mrb_nil_value();       /* not reached */
}

/* 15.2.8.3.1  */
/*
 * call-seq:
 *   fix + numeric  ->  numeric_result
 *
 * Performs addition: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */
static mrb_value
fix_plus(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);

  return fixnum_plus(mrb, self, other);
}

static mrb_value
fixnum_minus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;

  a = mrb_fixnum(x);
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    b = mrb_fixnum(y);
    if (mrb_int_sub_overflow(a, b, &c)) {
#ifndef MRB_WITHOUT_FLOAT
      return mrb_float_value(mrb, (mrb_float)a - (mrb_float)b);
#endif
    }
    return mrb_fixnum_value(c);
  }
#ifdef MRB_WITHOUT_FLOAT
  mrb_raise(mrb, E_TYPE_ERROR, "non fixnum value");
#else
  return mrb_float_value(mrb, (mrb_float)a - mrb_to_flo(mrb, y));
#endif
}

MRB_API mrb_value
mrb_num_minus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  if (mrb_fixnum_p(x)) {
    return fixnum_minus(mrb, x, y);
  }
#ifndef MRB_WITHOUT_FLOAT
  if (mrb_float_p(x)) {
    return mrb_float_value(mrb, mrb_float(x) - mrb_to_flo(mrb, y));
  }
#endif
  mrb_raise(mrb, E_TYPE_ERROR, "no number subtraction");
  return mrb_nil_value();       /* not reached */
}

/* 15.2.8.3.2  */
/* 15.2.8.3.16 */
/*
 * call-seq:
 *   fix - numeric  ->  numeric_result
 *
 * Performs subtraction: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */
static mrb_value
fix_minus(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);

  return fixnum_minus(mrb, self, other);
}


MRB_API mrb_value
mrb_fixnum_to_str(mrb_state *mrb, mrb_value x, mrb_int base)
{
  char buf[MRB_INT_BIT+1];
  char *b = buf + sizeof buf;
  mrb_int val = mrb_fixnum(x);
  mrb_value str;

  if (base < 2 || 36 < base) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid radix %i", base);
  }

  if (val == 0) {
    *--b = '0';
  }
  else if (val < 0) {
    do {
      *--b = mrb_digitmap[-(val % base)];
    } while (val /= base);
    *--b = '-';
  }
  else {
    do {
      *--b = mrb_digitmap[(int)(val % base)];
    } while (val /= base);
  }

  str = mrb_str_new(mrb, b, buf + sizeof(buf) - b);
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

/* 15.2.8.3.25 */
/*
 *  call-seq:
 *     fix.to_s(base=10)  ->  string
 *
 *  Returns a string containing the representation of <i>fix</i> radix
 *  <i>base</i> (between 2 and 36).
 *
 *     12345.to_s       #=> "12345"
 *     12345.to_s(2)    #=> "11000000111001"
 *     12345.to_s(8)    #=> "30071"
 *     12345.to_s(10)   #=> "12345"
 *     12345.to_s(16)   #=> "3039"
 *     12345.to_s(36)   #=> "9ix"
 *
 */
static mrb_value
fix_to_s(mrb_state *mrb, mrb_value self)
{
  mrb_int base = 10;

  mrb_get_args(mrb, "|i", &base);
  return mrb_fixnum_to_str(mrb, self, base);
}

/* compare two numbers: (1:0:-1; -2 for error) */
static mrb_int
cmpnum(mrb_state *mrb, mrb_value v1, mrb_value v2)
{
#ifdef MRB_WITHOUT_FLOAT
  mrb_int x, y;
#else
  mrb_float x, y;
#endif

#ifdef MRB_WITHOUT_FLOAT
  x = mrb_fixnum(v1);
#else
  x = mrb_to_flo(mrb, v1);
#endif
  switch (mrb_type(v2)) {
  case MRB_TT_FIXNUM:
#ifdef MRB_WITHOUT_FLOAT
    y = mrb_fixnum(v2);
#else
    y = (mrb_float)mrb_fixnum(v2);
#endif
    break;
#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
    y = mrb_float(v2);
    break;
#endif
  default:
    return -2;
  }
  if (x > y)
    return 1;
  else {
    if (x < y)
      return -1;
    return 0;
  }
}

/* 15.2.9.3.6  */
/*
 * call-seq:
 *     self.f <=> other.f    => -1, 0, +1, or nil
 *             <  => -1
 *             =  =>  0
 *             >  => +1
 *  Comparison---Returns -1, 0, or +1 depending on whether <i>fix</i> is
 *  less than, equal to, or greater than <i>numeric</i>. This is the
 *  basis for the tests in <code>Comparable</code>. When the operands are
 *  not comparable, it returns nil instead of raising an exception.
 */
static mrb_value
integral_cmp(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  mrb_int n;

  n = cmpnum(mrb, self, other);
  if (n == -2) return mrb_nil_value();
  return mrb_fixnum_value(n);
}

static mrb_noreturn void
cmperr(mrb_state *mrb, mrb_value v1, mrb_value v2)
{
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "comparison of %t with %t failed", v1, v2);
}

static mrb_value
integral_lt(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  mrb_int n;

  n = cmpnum(mrb, self, other);
  if (n == -2) cmperr(mrb, self, other);
  if (n < 0) return mrb_true_value();
  return mrb_false_value();
}

static mrb_value
integral_le(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  mrb_int n;

  n = cmpnum(mrb, self, other);
  if (n == -2) cmperr(mrb, self, other);
  if (n <= 0) return mrb_true_value();
  return mrb_false_value();
}

static mrb_value
integral_gt(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  mrb_int n;

  n = cmpnum(mrb, self, other);
  if (n == -2) cmperr(mrb, self, other);
  if (n > 0) return mrb_true_value();
  return mrb_false_value();
}

static mrb_value
integral_ge(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  mrb_int n;

  n = cmpnum(mrb, self, other);
  if (n == -2) cmperr(mrb, self, other);
  if (n >= 0) return mrb_true_value();
  return mrb_false_value();
}

MRB_API mrb_int
mrb_cmp(mrb_state *mrb, mrb_value obj1, mrb_value obj2)
{
  mrb_value v;

  switch (mrb_type(obj1)) {
  case MRB_TT_FIXNUM:
  case MRB_TT_FLOAT:
    return cmpnum(mrb, obj1, obj2);
  case MRB_TT_STRING:
    if (!mrb_string_p(obj2))
      return -2;
    return mrb_str_cmp(mrb, obj1, obj2);
  default:
    v = mrb_funcall(mrb, obj1, "<=>", 1, obj2);
    if (mrb_nil_p(v) || !mrb_fixnum_p(v))
      return -2;
    return mrb_fixnum(v);
  }
}

static mrb_value
num_finite_p(mrb_state *mrb, mrb_value self)
{
  return mrb_true_value();
}

static mrb_value
num_infinite_p(mrb_state *mrb, mrb_value self)
{
  return mrb_false_value();
}

/* 15.2.9.3.1  */
/*
 * call-seq:
 *   float + other  ->  float
 *
 * Returns a new float which is the sum of <code>float</code>
 * and <code>other</code>.
 */
#ifndef MRB_WITHOUT_FLOAT
static mrb_value
flo_plus(mrb_state *mrb, mrb_value x)
{
  mrb_value y = mrb_get_arg1(mrb);

  return mrb_float_value(mrb, mrb_float(x) + mrb_to_flo(mrb, y));
}
#endif

/* ------------------------------------------------------------------------*/
void
mrb_init_numeric(mrb_state *mrb)
{
  struct RClass *numeric, *integer, *fixnum, *integral;
#ifndef MRB_WITHOUT_FLOAT
  struct RClass *fl;
#endif

  integral = mrb_define_module(mrb, "Integral");
  mrb_define_method(mrb, integral,"**",       integral_pow,    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,"/",        integral_div,    MRB_ARGS_REQ(1)); /* 15.2.{8,9}.3.6  */
  mrb_define_method(mrb, integral,"quo",      integral_div,    MRB_ARGS_REQ(1)); /* 15.2.7.4.5 (x) */
  mrb_define_method(mrb, integral,"div",      integral_idiv,   MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,"<=>",      integral_cmp,    MRB_ARGS_REQ(1)); /* 15.2.{8,9}.3.1  */
  mrb_define_method(mrb, integral,"<",        integral_lt,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,"<=",       integral_le,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,">",        integral_gt,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,">=",       integral_ge,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, integral,"__coerce_step_counter", integral_coerce_step_counter, MRB_ARGS_REQ(2));

  /* Numeric Class */
  numeric = mrb_define_class(mrb, "Numeric",  mrb->object_class);                /* 15.2.7 */
  mrb_define_method(mrb, numeric, "finite?",  num_finite_p,    MRB_ARGS_NONE());
  mrb_define_method(mrb, numeric, "infinite?",num_infinite_p,  MRB_ARGS_NONE());

  /* Integer Class */
  integer = mrb_define_class(mrb, "Integer",  numeric);                          /* 15.2.8 */
  MRB_SET_INSTANCE_TT(integer, MRB_TT_FIXNUM);
  mrb_undef_class_method(mrb, integer, "new");
  mrb_define_method(mrb, integer, "to_i",     int_to_i,        MRB_ARGS_NONE()); /* 15.2.8.3.24 */
  mrb_define_method(mrb, integer, "to_int",   int_to_i,        MRB_ARGS_NONE());
#ifndef MRB_WITHOUT_FLOAT
  mrb_define_method(mrb, integer, "ceil",     int_to_i,        MRB_ARGS_NONE()); /* 15.2.8.3.8 (x) */
  mrb_define_method(mrb, integer, "floor",    int_to_i,        MRB_ARGS_NONE()); /* 15.2.8.3.10 (x) */
  mrb_define_method(mrb, integer, "round",    int_to_i,        MRB_ARGS_NONE()); /* 15.2.8.3.12 (x) */
  mrb_define_method(mrb, integer, "truncate", int_to_i,        MRB_ARGS_NONE()); /* 15.2.8.3.15 (x) */
#endif

  /* Fixnum Class */
  mrb->fixnum_class = fixnum = mrb_define_class(mrb, "Fixnum", integer);
  mrb_define_method(mrb, fixnum,  "+",        fix_plus,        MRB_ARGS_REQ(1)); /* 15.2.8.3.1  */
  mrb_define_method(mrb, fixnum,  "-",        fix_minus,       MRB_ARGS_REQ(1)); /* 15.2.8.3.2  */
  mrb_define_method(mrb, fixnum,  "*",        fix_mul,         MRB_ARGS_REQ(1)); /* 15.2.8.3.3  */
  mrb_define_method(mrb, fixnum,  "%",        fix_mod,         MRB_ARGS_REQ(1)); /* 15.2.8.3.5  */
  mrb_define_method(mrb, fixnum,  "==",       fix_equal,       MRB_ARGS_REQ(1)); /* 15.2.8.3.7  */
  mrb_define_method(mrb, fixnum,  "~",        fix_rev,         MRB_ARGS_NONE()); /* 15.2.8.3.8  */
  mrb_define_method(mrb, fixnum,  "&",        fix_and,         MRB_ARGS_REQ(1)); /* 15.2.8.3.9  */
  mrb_define_method(mrb, fixnum,  "|",        fix_or,          MRB_ARGS_REQ(1)); /* 15.2.8.3.10 */
  mrb_define_method(mrb, fixnum,  "^",        fix_xor,         MRB_ARGS_REQ(1)); /* 15.2.8.3.11 */
  mrb_define_method(mrb, fixnum,  "<<",       fix_lshift,      MRB_ARGS_REQ(1)); /* 15.2.8.3.12 */
  mrb_define_method(mrb, fixnum,  ">>",       fix_rshift,      MRB_ARGS_REQ(1)); /* 15.2.8.3.13 */
  mrb_define_method(mrb, fixnum,  "eql?",     fix_eql,         MRB_ARGS_REQ(1)); /* 15.2.8.3.16 */
#ifndef MRB_WITHOUT_FLOAT
  mrb_define_method(mrb, fixnum,  "to_f",     fix_to_f,        MRB_ARGS_NONE()); /* 15.2.8.3.23 */
#endif
  mrb_define_method(mrb, fixnum,  "to_s",     fix_to_s,        MRB_ARGS_OPT(1)); /* 15.2.8.3.25 */
  mrb_define_method(mrb, fixnum,  "inspect",  fix_to_s,        MRB_ARGS_OPT(1));
  mrb_define_method(mrb, fixnum,  "divmod",   fix_divmod,      MRB_ARGS_REQ(1)); /* 15.2.8.3.30 (x) */

#ifndef MRB_WITHOUT_FLOAT
  /* Float Class */
  mrb->float_class = fl = mrb_define_class(mrb, "Float", numeric);                 /* 15.2.9 */
  MRB_SET_INSTANCE_TT(fl, MRB_TT_FLOAT);
  mrb_undef_class_method(mrb,  fl, "new");
  mrb_define_method(mrb, fl,      "+",         flo_plus,       MRB_ARGS_REQ(1)); /* 15.2.9.3.1  */
  mrb_define_method(mrb, fl,      "-",         flo_minus,      MRB_ARGS_REQ(1)); /* 15.2.9.3.2  */
  mrb_define_method(mrb, fl,      "*",         flo_mul,        MRB_ARGS_REQ(1)); /* 15.2.9.3.3  */
  mrb_define_method(mrb, fl,      "%",         flo_mod,        MRB_ARGS_REQ(1)); /* 15.2.9.3.5  */
  mrb_define_method(mrb, fl,      "==",        flo_eq,         MRB_ARGS_REQ(1)); /* 15.2.9.3.7  */
  mrb_define_method(mrb, fl,      "~",         flo_rev,        MRB_ARGS_NONE());
  mrb_define_method(mrb, fl,      "&",         flo_and,        MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      "|",         flo_or,         MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      "^",         flo_xor,        MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      ">>",        flo_rshift,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      "<<",        flo_lshift,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      "ceil",      flo_ceil,       MRB_ARGS_NONE()); /* 15.2.9.3.8  */
  mrb_define_method(mrb, fl,      "finite?",   flo_finite_p,   MRB_ARGS_NONE()); /* 15.2.9.3.9  */
  mrb_define_method(mrb, fl,      "floor",     flo_floor,      MRB_ARGS_NONE()); /* 15.2.9.3.10 */
  mrb_define_method(mrb, fl,      "infinite?", flo_infinite_p, MRB_ARGS_NONE()); /* 15.2.9.3.11 */
  mrb_define_method(mrb, fl,      "round",     flo_round,      MRB_ARGS_OPT(1)); /* 15.2.9.3.12 */
  mrb_define_method(mrb, fl,      "to_f",      flo_to_f,       MRB_ARGS_NONE()); /* 15.2.9.3.13 */
  mrb_define_method(mrb, fl,      "to_i",      flo_truncate,   MRB_ARGS_NONE()); /* 15.2.9.3.14 */
  mrb_define_method(mrb, fl,      "to_int",    flo_truncate,   MRB_ARGS_NONE());
  mrb_define_method(mrb, fl,      "truncate",  flo_truncate,   MRB_ARGS_NONE()); /* 15.2.9.3.15 */
  mrb_define_method(mrb, fl,      "divmod",    flo_divmod,     MRB_ARGS_REQ(1));
  mrb_define_method(mrb, fl,      "eql?",      flo_eql,        MRB_ARGS_REQ(1)); /* 15.2.8.3.16 */

  mrb_define_method(mrb, fl,      "to_s",      flo_to_s,       MRB_ARGS_NONE()); /* 15.2.9.3.16(x) */
  mrb_define_method(mrb, fl,      "inspect",   flo_to_s,       MRB_ARGS_NONE());
  mrb_define_method(mrb, fl,      "nan?",      flo_nan_p,      MRB_ARGS_NONE());

#ifdef INFINITY
  mrb_define_const(mrb, fl, "INFINITY", mrb_float_value(mrb, INFINITY));
#endif
#ifdef NAN
  mrb_define_const(mrb, fl, "NAN", mrb_float_value(mrb, NAN));
#endif

  mrb_include_module(mrb, fl, integral);
#endif
}
/*
** object.c - Object, NilClass, TrueClass, FalseClass class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include <mruby/class.h>

MRB_API mrb_bool
mrb_obj_eq(mrb_state *mrb, mrb_value v1, mrb_value v2)
{
  if (mrb_type(v1) != mrb_type(v2)) return FALSE;
  switch (mrb_type(v1)) {
  case MRB_TT_TRUE:
    return TRUE;

  case MRB_TT_FALSE:
  case MRB_TT_FIXNUM:
    return (mrb_fixnum(v1) == mrb_fixnum(v2));
  case MRB_TT_SYMBOL:
    return (mrb_symbol(v1) == mrb_symbol(v2));

#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
    return (mrb_float(v1) == mrb_float(v2));
#endif

  default:
    return (mrb_ptr(v1) == mrb_ptr(v2));
  }
}

MRB_API mrb_bool
mrb_obj_equal(mrb_state *mrb, mrb_value v1, mrb_value v2)
{
  /* temporary definition */
  return mrb_obj_eq(mrb, v1, v2);
}

MRB_API mrb_bool
mrb_equal(mrb_state *mrb, mrb_value obj1, mrb_value obj2)
{
  mrb_value result;

  if (mrb_obj_eq(mrb, obj1, obj2)) return TRUE;
#ifndef MRB_WITHOUT_FLOAT
  /* value mixing with integer and float */
  if (mrb_fixnum_p(obj1)) {
    if (mrb_float_p(obj2) && (mrb_float)mrb_fixnum(obj1) == mrb_float(obj2))
      return TRUE;
  }
  else if (mrb_float_p(obj1)) {
    if (mrb_fixnum_p(obj2) && mrb_float(obj1) == (mrb_float)mrb_fixnum(obj2))
      return TRUE;
  }
#endif
  result = mrb_funcall(mrb, obj1, "==", 1, obj2);
  if (mrb_test(result)) return TRUE;
  return FALSE;
}

/*
 * Document-class: NilClass
 *
 *  The class of the singleton object <code>nil</code>.
 */

/* 15.2.4.3.4  */
/*
 * call_seq:
 *   nil.nil?               -> true
 *
 * Only the object <i>nil</i> responds <code>true</code> to <code>nil?</code>.
 */

static mrb_value
mrb_true(mrb_state *mrb, mrb_value obj)
{
  return mrb_true_value();
}

/* 15.2.4.3.5  */
/*
 *  call-seq:
 *     nil.to_s    -> ""
 *
 *  Always returns the empty string.
 */

static mrb_value
nil_to_s(mrb_state *mrb, mrb_value obj)
{
  mrb_value str = mrb_str_new_frozen(mrb, 0, 0);
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

static mrb_value
nil_inspect(mrb_state *mrb, mrb_value obj)
{
  mrb_value str = mrb_str_new_lit_frozen(mrb, "nil");
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

/***********************************************************************
 *  Document-class: TrueClass
 *
 *  The global value <code>true</code> is the only instance of class
 *  <code>TrueClass</code> and represents a logically true value in
 *  boolean expressions. The class provides operators allowing
 *  <code>true</code> to be used in logical expressions.
 */

/* 15.2.5.3.1  */
/*
 *  call-seq:
 *     true & obj    -> true or false
 *
 *  And---Returns <code>false</code> if <i>obj</i> is
 *  <code>nil</code> or <code>false</code>, <code>true</code> otherwise.
 */

static mrb_value
true_and(mrb_state *mrb, mrb_value obj)
{
  mrb_bool obj2;

  mrb_get_args(mrb, "b", &obj2);

  return mrb_bool_value(obj2);
}

/* 15.2.5.3.2  */
/*
 *  call-seq:
 *     true ^ obj   -> !obj
 *
 *  Exclusive Or---Returns <code>true</code> if <i>obj</i> is
 *  <code>nil</code> or <code>false</code>, <code>false</code>
 *  otherwise.
 */

static mrb_value
true_xor(mrb_state *mrb, mrb_value obj)
{
  mrb_bool obj2;

  mrb_get_args(mrb, "b", &obj2);
  return mrb_bool_value(!obj2);
}

/* 15.2.5.3.3  */
/*
 * call-seq:
 *   true.to_s   ->  "true"
 *
 * The string representation of <code>true</code> is "true".
 */

static mrb_value
true_to_s(mrb_state *mrb, mrb_value obj)
{
  mrb_value str = mrb_str_new_lit_frozen(mrb, "true");
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

/* 15.2.5.3.4  */
/*
 *  call-seq:
 *     true | obj   -> true
 *
 *  Or---Returns <code>true</code>. As <i>anObject</i> is an argument to
 *  a method call, it is always evaluated; there is no short-circuit
 *  evaluation in this case.
 *
 *     true |  puts("or")
 *     true || puts("logical or")
 *
 *  <em>produces:</em>
 *
 *     or
 */

static mrb_value
true_or(mrb_state *mrb, mrb_value obj)
{
  return mrb_true_value();
}

/*
 *  Document-class: FalseClass
 *
 *  The global value <code>false</code> is the only instance of class
 *  <code>FalseClass</code> and represents a logically false value in
 *  boolean expressions. The class provides operators allowing
 *  <code>false</code> to participate correctly in logical expressions.
 *
 */

/* 15.2.4.3.1  */
/* 15.2.6.3.1  */
/*
 *  call-seq:
 *     false & obj   -> false
 *     nil & obj     -> false
 *
 *  And---Returns <code>false</code>. <i>obj</i> is always
 *  evaluated as it is the argument to a method call---there is no
 *  short-circuit evaluation in this case.
 */

static mrb_value
false_and(mrb_state *mrb, mrb_value obj)
{
  return mrb_false_value();
}

/* 15.2.4.3.2  */
/* 15.2.6.3.2  */
/*
 *  call-seq:
 *     false ^ obj    -> true or false
 *     nil   ^ obj    -> true or false
 *
 *  Exclusive Or---If <i>obj</i> is <code>nil</code> or
 *  <code>false</code>, returns <code>false</code>; otherwise, returns
 *  <code>true</code>.
 *
 */

static mrb_value
false_xor(mrb_state *mrb, mrb_value obj)
{
  mrb_bool obj2;

  mrb_get_args(mrb, "b", &obj2);
  return mrb_bool_value(obj2);
}

/* 15.2.4.3.3  */
/* 15.2.6.3.4  */
/*
 *  call-seq:
 *     false | obj   ->   true or false
 *     nil   | obj   ->   true or false
 *
 *  Or---Returns <code>false</code> if <i>obj</i> is
 *  <code>nil</code> or <code>false</code>; <code>true</code> otherwise.
 */

static mrb_value
false_or(mrb_state *mrb, mrb_value obj)
{
  mrb_bool obj2;

  mrb_get_args(mrb, "b", &obj2);
  return mrb_bool_value(obj2);
}

/* 15.2.6.3.3  */
/*
 * call-seq:
 *   false.to_s   ->  "false"
 *
 * 'nuf said...
 */

static mrb_value
false_to_s(mrb_state *mrb, mrb_value obj)
{
  mrb_value str = mrb_str_new_lit_frozen(mrb, "false");
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

void
mrb_init_object(mrb_state *mrb)
{
  struct RClass *n;
  struct RClass *t;
  struct RClass *f;

  mrb->nil_class   = n = mrb_define_class(mrb, "NilClass",   mrb->object_class);
  MRB_SET_INSTANCE_TT(n, MRB_TT_TRUE);
  mrb_undef_class_method(mrb, n, "new");
  mrb_define_method(mrb, n, "&",    false_and,      MRB_ARGS_REQ(1));  /* 15.2.4.3.1  */
  mrb_define_method(mrb, n, "^",    false_xor,      MRB_ARGS_REQ(1));  /* 15.2.4.3.2  */
  mrb_define_method(mrb, n, "|",    false_or,       MRB_ARGS_REQ(1));  /* 15.2.4.3.3  */
  mrb_define_method(mrb, n, "nil?", mrb_true,       MRB_ARGS_NONE());  /* 15.2.4.3.4  */
  mrb_define_method(mrb, n, "to_s", nil_to_s,       MRB_ARGS_NONE());  /* 15.2.4.3.5  */
  mrb_define_method(mrb, n, "inspect", nil_inspect, MRB_ARGS_NONE());

  mrb->true_class  = t = mrb_define_class(mrb, "TrueClass",  mrb->object_class);
  MRB_SET_INSTANCE_TT(t, MRB_TT_TRUE);
  mrb_undef_class_method(mrb, t, "new");
  mrb_define_method(mrb, t, "&",    true_and,       MRB_ARGS_REQ(1));  /* 15.2.5.3.1  */
  mrb_define_method(mrb, t, "^",    true_xor,       MRB_ARGS_REQ(1));  /* 15.2.5.3.2  */
  mrb_define_method(mrb, t, "to_s", true_to_s,      MRB_ARGS_NONE());  /* 15.2.5.3.3  */
  mrb_define_method(mrb, t, "|",    true_or,        MRB_ARGS_REQ(1));  /* 15.2.5.3.4  */
  mrb_define_method(mrb, t, "inspect", true_to_s,   MRB_ARGS_NONE());

  mrb->false_class = f = mrb_define_class(mrb, "FalseClass", mrb->object_class);
  MRB_SET_INSTANCE_TT(f, MRB_TT_TRUE);
  mrb_undef_class_method(mrb, f, "new");
  mrb_define_method(mrb, f, "&",    false_and,      MRB_ARGS_REQ(1));  /* 15.2.6.3.1  */
  mrb_define_method(mrb, f, "^",    false_xor,      MRB_ARGS_REQ(1));  /* 15.2.6.3.2  */
  mrb_define_method(mrb, f, "to_s", false_to_s,     MRB_ARGS_NONE());  /* 15.2.6.3.3  */
  mrb_define_method(mrb, f, "|",    false_or,       MRB_ARGS_REQ(1));  /* 15.2.6.3.4  */
  mrb_define_method(mrb, f, "inspect", false_to_s,  MRB_ARGS_NONE());
}

static mrb_value
convert_type(mrb_state *mrb, mrb_value val, const char *tname, const char *method, mrb_bool raise)
{
  mrb_sym m = 0;

  m = mrb_intern_cstr(mrb, method);
  if (!mrb_respond_to(mrb, val, m)) {
    if (raise) {
      mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %Y into %s", val, tname);
    }
    return mrb_nil_value();
  }
  return mrb_funcall_argv(mrb, val, m, 0, 0);
}

MRB_API mrb_value
mrb_convert_type(mrb_state *mrb, mrb_value val, enum mrb_vtype type, const char *tname, const char *method)
{
  mrb_value v;

  if (mrb_type(val) == type) return val;
  v = convert_type(mrb, val, tname, method, TRUE);
  if (mrb_type(v) != type) {
    mrb_raisef(mrb, E_TYPE_ERROR, "%v cannot be converted to %s by #%s", val, tname, method);
  }
  return v;
}

MRB_API mrb_value
mrb_check_convert_type(mrb_state *mrb, mrb_value val, enum mrb_vtype type, const char *tname, const char *method)
{
  mrb_value v;

  if (mrb_type(val) == type && type != MRB_TT_DATA && type != MRB_TT_ISTRUCT) return val;
  v = convert_type(mrb, val, tname, method, FALSE);
  if (mrb_nil_p(v) || mrb_type(v) != type) return mrb_nil_value();
  return v;
}

static const struct types {
  unsigned char type;
  const char *name;
} builtin_types[] = {
/*    {MRB_TT_NIL,  "nil"}, */
  {MRB_TT_FALSE,  "false"},
  {MRB_TT_TRUE,   "true"},
  {MRB_TT_FIXNUM, "Fixnum"},
  {MRB_TT_SYMBOL, "Symbol"},  /* :symbol */
  {MRB_TT_MODULE, "Module"},
  {MRB_TT_OBJECT, "Object"},
  {MRB_TT_CLASS,  "Class"},
  {MRB_TT_ICLASS, "iClass"},  /* internal use: mixed-in module holder */
  {MRB_TT_SCLASS, "SClass"},
  {MRB_TT_PROC,   "Proc"},
#ifndef MRB_WITHOUT_FLOAT
  {MRB_TT_FLOAT,  "Float"},
#endif
  {MRB_TT_ARRAY,  "Array"},
  {MRB_TT_HASH,   "Hash"},
  {MRB_TT_STRING, "String"},
  {MRB_TT_RANGE,  "Range"},
/*    {MRB_TT_BIGNUM,  "Bignum"}, */
  {MRB_TT_DATA,   "Data"},  /* internal use: wrapped C pointers */
/*    {MRB_TT_VARMAP,  "Varmap"}, */ /* internal use: dynamic variables */
/*    {MRB_TT_NODE,  "Node"}, */ /* internal use: syntax tree node */
/*    {MRB_TT_UNDEF,  "undef"}, */ /* internal use: #undef; should not happen */
  {MRB_TT_MAXDEFINE,  0}
};

MRB_API void
mrb_check_type(mrb_state *mrb, mrb_value x, enum mrb_vtype t)
{
  const struct types *type = builtin_types;
  enum mrb_vtype xt;

  xt = mrb_type(x);
  if ((xt != t) || (xt == MRB_TT_DATA) || (xt == MRB_TT_ISTRUCT)) {
    while (type->type < MRB_TT_MAXDEFINE) {
      if (type->type == t) {
        const char *etype;

        if (mrb_nil_p(x)) {
          etype = "nil";
        }
        else if (mrb_fixnum_p(x)) {
          etype = "Fixnum";
        }
        else if (mrb_symbol_p(x)) {
          etype = "Symbol";
        }
        else if (mrb_immediate_p(x)) {
          etype = RSTRING_PTR(mrb_obj_as_string(mrb, x));
        }
        else {
          etype = mrb_obj_classname(mrb, x);
        }
        mrb_raisef(mrb, E_TYPE_ERROR, "wrong argument type %s (expected %s)",
                   etype, type->name);
      }
      type++;
    }
    mrb_raisef(mrb, E_TYPE_ERROR, "unknown type %d (%d given)", t, mrb_type(x));
  }
}

/* 15.3.1.3.46 */
/*
 *  call-seq:
 *     obj.to_s    => string
 *
 *  Returns a string representing <i>obj</i>. The default
 *  <code>to_s</code> prints the object's class and an encoding of the
 *  object id. As a special case, the top-level object that is the
 *  initial execution context of Ruby programs returns "main."
 */

MRB_API mrb_value
mrb_any_to_s(mrb_state *mrb, mrb_value obj)
{
  mrb_value str = mrb_str_new_capa(mrb, 20);
  const char *cname = mrb_obj_classname(mrb, obj);

  mrb_str_cat_lit(mrb, str, "#<");
  mrb_str_cat_cstr(mrb, str, cname);
  if (!mrb_immediate_p(obj)) {
    mrb_str_cat_lit(mrb, str, ":");
    mrb_str_cat_str(mrb, str, mrb_ptr_to_str(mrb, mrb_ptr(obj)));
  }
  mrb_str_cat_lit(mrb, str, ">");

  return str;
}

/*
 *  call-seq:
 *     obj.is_a?(class)       => true or false
 *     obj.kind_of?(class)    => true or false
 *
 *  Returns <code>true</code> if <i>class</i> is the class of
 *  <i>obj</i>, or if <i>class</i> is one of the superclasses of
 *  <i>obj</i> or modules included in <i>obj</i>.
 *
 *     module M;    end
 *     class A
 *       include M
 *     end
 *     class B < A; end
 *     class C < B; end
 *     b = B.new
 *     b.instance_of? A   #=> false
 *     b.instance_of? B   #=> true
 *     b.instance_of? C   #=> false
 *     b.instance_of? M   #=> false
 *     b.kind_of? A       #=> true
 *     b.kind_of? B       #=> true
 *     b.kind_of? C       #=> false
 *     b.kind_of? M       #=> true
 */

MRB_API mrb_bool
mrb_obj_is_kind_of(mrb_state *mrb, mrb_value obj, struct RClass *c)
{
  struct RClass *cl = mrb_class(mrb, obj);

  switch (c->tt) {
    case MRB_TT_MODULE:
    case MRB_TT_CLASS:
    case MRB_TT_ICLASS:
    case MRB_TT_SCLASS:
      break;

    default:
      mrb_raise(mrb, E_TYPE_ERROR, "class or module required");
  }

  MRB_CLASS_ORIGIN(c);
  while (cl) {
    if (cl == c || cl->mt == c->mt)
      return TRUE;
    cl = cl->super;
  }
  return FALSE;
}

MRB_API mrb_value
mrb_to_int(mrb_state *mrb, mrb_value val)
{

  if (!mrb_fixnum_p(val)) {
#ifndef MRB_WITHOUT_FLOAT
    if (mrb_float_p(val)) {
      return mrb_flo_to_fixnum(mrb, val);
    }
#endif
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %Y to Integer", val);
  }
  return val;
}

MRB_API mrb_value
mrb_convert_to_integer(mrb_state *mrb, mrb_value val, mrb_int base)
{
  mrb_value tmp;

  if (mrb_nil_p(val)) {
    if (base != 0) goto arg_error;
    mrb_raise(mrb, E_TYPE_ERROR, "can't convert nil into Integer");
  }
  switch (mrb_type(val)) {
#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      if (base != 0) goto arg_error;
      return mrb_flo_to_fixnum(mrb, val);
#endif

    case MRB_TT_FIXNUM:
      if (base != 0) goto arg_error;
      return val;

    case MRB_TT_STRING:
    string_conv:
      return mrb_str_to_inum(mrb, val, base, TRUE);

    default:
      break;
  }
  if (base != 0) {
    tmp = mrb_check_string_type(mrb, val);
    if (!mrb_nil_p(tmp)) {
      val = tmp;
      goto string_conv;
    }
arg_error:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "base specified for non string value");
  }
  /* to raise TypeError */
  return mrb_to_int(mrb, val);
}

MRB_API mrb_value
mrb_Integer(mrb_state *mrb, mrb_value val)
{
  return mrb_convert_to_integer(mrb, val, 0);
}

#ifndef MRB_WITHOUT_FLOAT
MRB_API mrb_value
mrb_Float(mrb_state *mrb, mrb_value val)
{
  if (mrb_nil_p(val)) {
    mrb_raise(mrb, E_TYPE_ERROR, "can't convert nil into Float");
  }
  switch (mrb_type(val)) {
    case MRB_TT_FIXNUM:
      return mrb_float_value(mrb, (mrb_float)mrb_fixnum(val));

    case MRB_TT_FLOAT:
      return val;

    case MRB_TT_STRING:
      return mrb_float_value(mrb, mrb_str_to_dbl(mrb, val, TRUE));

    default:
      return mrb_convert_type(mrb, val, MRB_TT_FLOAT, "Float", "to_f");
  }
}
#endif

MRB_API mrb_value
mrb_to_str(mrb_state *mrb, mrb_value val)
{
  return mrb_ensure_string_type(mrb, val);
}

/* obsolete: use mrb_ensure_string_type() instead */
MRB_API mrb_value
mrb_string_type(mrb_state *mrb, mrb_value str)
{
  return mrb_ensure_string_type(mrb, str);
}

MRB_API mrb_value
mrb_ensure_string_type(mrb_state *mrb, mrb_value str)
{
  if (!mrb_string_p(str)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "%Y cannot be converted to String", str);
  }
  return str;
}

MRB_API mrb_value
mrb_check_string_type(mrb_state *mrb, mrb_value str)
{
  if (!mrb_string_p(str)) return mrb_nil_value();
  return str;
}

MRB_API mrb_value
mrb_ensure_array_type(mrb_state *mrb, mrb_value ary)
{
  if (!mrb_array_p(ary)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "%Y cannot be converted to Array", ary);
  }
  return ary;
}

MRB_API mrb_value
mrb_check_array_type(mrb_state *mrb, mrb_value ary)
{
  if (!mrb_array_p(ary)) return mrb_nil_value();
  return ary;
}

MRB_API mrb_value
mrb_ensure_hash_type(mrb_state *mrb, mrb_value hash)
{
  if (!mrb_hash_p(hash)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "%Y cannot be converted to Hash", hash);
  }
  return hash;
}

MRB_API mrb_value
mrb_check_hash_type(mrb_state *mrb, mrb_value hash)
{
  if (!mrb_hash_p(hash)) return mrb_nil_value();
  return hash;
}

MRB_API mrb_value
mrb_inspect(mrb_state *mrb, mrb_value obj)
{
  return mrb_obj_as_string(mrb, mrb_funcall(mrb, obj, "inspect", 0));
}

MRB_API mrb_bool
mrb_eql(mrb_state *mrb, mrb_value obj1, mrb_value obj2)
{
  if (mrb_obj_eq(mrb, obj1, obj2)) return TRUE;
  return mrb_test(mrb_funcall(mrb, obj1, "eql?", 1, obj2));
}
/*
** pool.c - memory pool
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <mruby.h>

/* configuration section */
/* allocated memory address should be multiple of POOL_ALIGNMENT */
/* or undef it if alignment does not matter */
#ifndef POOL_ALIGNMENT
#if INTPTR_MAX == INT64_MAX
#define POOL_ALIGNMENT 8
#else
#define POOL_ALIGNMENT 4
#endif
#endif
/* page size of memory pool */
#ifndef POOL_PAGE_SIZE
#define POOL_PAGE_SIZE 16000
#endif
/* end of configuration section */

/* Disable MSVC warning "C4200: nonstandard extension used: zero-sized array
 * in struct/union" when in C++ mode */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200)
#endif

struct mrb_pool_page {
  struct mrb_pool_page *next;
  size_t offset;
  size_t len;
  void *last;
  char page[];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct mrb_pool {
  mrb_state *mrb;
  struct mrb_pool_page *pages;
};

#undef TEST_POOL
#ifdef TEST_POOL

#define mrb_malloc_simple(m,s) malloc(s)
#define mrb_free(m,p) free(p)
#endif

#ifdef POOL_ALIGNMENT
#  define ALIGN_PADDING(x) ((SIZE_MAX - (x) + 1) & (POOL_ALIGNMENT - 1))
#else
#  define ALIGN_PADDING(x) (0)
#endif

MRB_API mrb_pool*
mrb_pool_open(mrb_state *mrb)
{
  mrb_pool *pool = (mrb_pool *)mrb_malloc_simple(mrb, sizeof(mrb_pool));

  if (pool) {
    pool->mrb = mrb;
    pool->pages = NULL;
  }

  return pool;
}

MRB_API void
mrb_pool_close(mrb_pool *pool)
{
  struct mrb_pool_page *page, *tmp;

  if (!pool) return;
  page = pool->pages;
  while (page) {
    tmp = page;
    page = page->next;
    mrb_free(pool->mrb, tmp);
  }
  mrb_free(pool->mrb, pool);
}

static struct mrb_pool_page*
page_alloc(mrb_pool *pool, size_t len)
{
  struct mrb_pool_page *page;

  if (len < POOL_PAGE_SIZE)
    len = POOL_PAGE_SIZE;
  page = (struct mrb_pool_page *)mrb_malloc_simple(pool->mrb, sizeof(struct mrb_pool_page)+len);
  if (page) {
    page->offset = 0;
    page->len = len;
  }

  return page;
}

MRB_API void*
mrb_pool_alloc(mrb_pool *pool, size_t len)
{
  struct mrb_pool_page *page;
  size_t n;

  if (!pool) return NULL;
  len += ALIGN_PADDING(len);
  page = pool->pages;
  while (page) {
    if (page->offset + len <= page->len) {
      n = page->offset;
      page->offset += len;
      page->last = (char*)page->page+n;
      return page->last;
    }
    page = page->next;
  }
  page = page_alloc(pool, len);
  if (!page) return NULL;
  page->offset = len;
  page->next = pool->pages;
  pool->pages = page;

  page->last = (void*)page->page;
  return page->last;
}

MRB_API mrb_bool
mrb_pool_can_realloc(mrb_pool *pool, void *p, size_t len)
{
  struct mrb_pool_page *page;

  if (!pool) return FALSE;
  len += ALIGN_PADDING(len);
  page = pool->pages;
  while (page) {
    if (page->last == p) {
      size_t beg;

      beg = (char*)p - page->page;
      if (beg + len > page->len) return FALSE;
      return TRUE;
    }
    page = page->next;
  }
  return FALSE;
}

MRB_API void*
mrb_pool_realloc(mrb_pool *pool, void *p, size_t oldlen, size_t newlen)
{
  struct mrb_pool_page *page;
  void *np;

  if (!pool) return NULL;
  oldlen += ALIGN_PADDING(oldlen);
  newlen += ALIGN_PADDING(newlen);
  page = pool->pages;
  while (page) {
    if (page->last == p) {
      size_t beg;

      beg = (char*)p - page->page;
      if (beg + oldlen != page->offset) break;
      if (beg + newlen > page->len) {
        page->offset = beg;
        break;
      }
      page->offset = beg + newlen;
      return p;
    }
    page = page->next;
  }
  np = mrb_pool_alloc(pool, newlen);
  if (np == NULL) {
    return NULL;
  }
  memcpy(np, p, oldlen);
  return np;
}

#ifdef TEST_POOL
int
main(void)
{
  int i, len = 250;
  mrb_pool *pool;
  void *p;

  pool = mrb_pool_open(NULL);
  p = mrb_pool_alloc(pool, len);
  for (i=1; i<20; i++) {
    printf("%p (len=%d) %ud\n", p, len, mrb_pool_can_realloc(pool, p, len*2));
    p = mrb_pool_realloc(pool, p, len, len*2);
    len *= 2;
  }
  mrb_pool_close(pool);
  return 0;
}
#endif
/*
** print.c - Kernel.#p
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <string.h>

#ifndef MRB_DISABLE_STDIO
static void
printcstr(const char *str, size_t len, FILE *stream)
{
  if (str) {
    fwrite(str, len, 1, stream);
    putc('\n', stream);
  }
}

static void
printstr(mrb_value obj, FILE *stream)
{
  if (mrb_string_p(obj)) {
    printcstr(RSTRING_PTR(obj), RSTRING_LEN(obj), stream);
  }
}
#else
# define printcstr(str, len, stream) (void)0
# define printstr(obj, stream) (void)0
#endif

void
mrb_core_init_printabort(void)
{
  static const char *str = "Failed mruby core initialization";
  printcstr(str, strlen(str), stdout);
}

MRB_API void
mrb_p(mrb_state *mrb, mrb_value obj)
{
  if (mrb_type(obj) == MRB_TT_EXCEPTION && mrb_obj_ptr(obj) == mrb->nomem_err) {
    static const char *str = "Out of memory";
    printcstr(str, strlen(str), stdout);
  }
  else {
    printstr(mrb_inspect(mrb, obj), stdout);
  }
}

MRB_API void
mrb_print_error(mrb_state *mrb)
{
  mrb_print_backtrace(mrb);
}

MRB_API void
mrb_show_version(mrb_state *mrb)
{
  printstr(mrb_const_get(mrb, mrb_obj_value(mrb->object_class), mrb_intern_lit(mrb, "MRUBY_DESCRIPTION")), stdout);
}

MRB_API void
mrb_show_copyright(mrb_state *mrb)
{
  printstr(mrb_const_get(mrb, mrb_obj_value(mrb->object_class), mrb_intern_lit(mrb, "MRUBY_COPYRIGHT")), stdout);
}
/*
** proc.c - Proc class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>
#include <mruby/data.h>

static const mrb_code call_iseq[] = {
  OP_CALL,
};

struct RProc*
mrb_proc_new(mrb_state *mrb, mrb_irep *irep)
{
  struct RProc *p;
  mrb_callinfo *ci = mrb->c->ci;

  p = (struct RProc*)mrb_obj_alloc(mrb, MRB_TT_PROC, mrb->proc_class);
  if (ci) {
    struct RClass *tc = NULL;

    if (ci->proc) {
      tc = MRB_PROC_TARGET_CLASS(ci->proc);
    }
    if (tc == NULL) {
      tc = ci->target_class;
    }
    p->upper = ci->proc;
    p->e.target_class = tc;
  }
  p->body.irep = irep;
  mrb_irep_incref(mrb, irep);

  return p;
}

static struct REnv*
env_new(mrb_state *mrb, mrb_int nlocals)
{
  struct REnv *e;
  mrb_callinfo *ci = mrb->c->ci;
  int bidx;

  e = (struct REnv*)mrb_obj_alloc(mrb, MRB_TT_ENV, NULL);
  MRB_ENV_SET_STACK_LEN(e, nlocals);
  bidx = ci->argc;
  if (ci->argc < 0) bidx = 2;
  else bidx += 1;
  MRB_ENV_SET_BIDX(e, bidx);
  e->mid = ci->mid;
  e->stack = mrb->c->stack;
  e->cxt = mrb->c;

  return e;
}

static void
closure_setup(mrb_state *mrb, struct RProc *p)
{
  mrb_callinfo *ci = mrb->c->ci;
  struct RProc *up = p->upper;
  struct REnv *e = NULL;

  if (ci && ci->env) {
    e = ci->env;
  }
  else if (up) {
    struct RClass *tc = MRB_PROC_TARGET_CLASS(p);

    e = env_new(mrb, up->body.irep->nlocals);
    ci->env = e;
    if (tc) {
      e->c = tc;
      mrb_field_write_barrier(mrb, (struct RBasic*)e, (struct RBasic*)tc);
    }
    if (MRB_PROC_ENV_P(up) && MRB_PROC_ENV(up)->cxt == NULL) {
      e->mid = MRB_PROC_ENV(up)->mid;
    }
  }
  if (e) {
    p->e.env = e;
    p->flags |= MRB_PROC_ENVSET;
    mrb_field_write_barrier(mrb, (struct RBasic*)p, (struct RBasic*)e);
  }
}

struct RProc*
mrb_closure_new(mrb_state *mrb, mrb_irep *irep)
{
  struct RProc *p = mrb_proc_new(mrb, irep);

  closure_setup(mrb, p);
  return p;
}

MRB_API struct RProc*
mrb_proc_new_cfunc(mrb_state *mrb, mrb_func_t func)
{
  struct RProc *p;

  p = (struct RProc*)mrb_obj_alloc(mrb, MRB_TT_PROC, mrb->proc_class);
  p->body.func = func;
  p->flags |= MRB_PROC_CFUNC_FL;
  p->upper = 0;
  p->e.target_class = 0;

  return p;
}

MRB_API struct RProc*
mrb_proc_new_cfunc_with_env(mrb_state *mrb, mrb_func_t func, mrb_int argc, const mrb_value *argv)
{
  struct RProc *p = mrb_proc_new_cfunc(mrb, func);
  struct REnv *e;
  int i;

  p->e.env = e = env_new(mrb, argc);
  p->flags |= MRB_PROC_ENVSET;
  mrb_field_write_barrier(mrb, (struct RBasic*)p, (struct RBasic*)e);
  MRB_ENV_UNSHARE_STACK(e);

  /* NOTE: Prevents keeping invalid addresses when NoMemoryError is raised from `mrb_malloc()`. */
  e->stack = NULL;
  MRB_ENV_SET_STACK_LEN(e, 0);

  e->stack = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value) * argc);
  MRB_ENV_SET_STACK_LEN(e, argc);

  if (argv) {
    for (i = 0; i < argc; ++i) {
      e->stack[i] = argv[i];
    }
  }
  else {
    for (i = 0; i < argc; ++i) {
      SET_NIL_VALUE(e->stack[i]);
    }
  }
  return p;
}

MRB_API struct RProc*
mrb_closure_new_cfunc(mrb_state *mrb, mrb_func_t func, int nlocals)
{
  return mrb_proc_new_cfunc_with_env(mrb, func, nlocals, NULL);
}

MRB_API mrb_value
mrb_proc_cfunc_env_get(mrb_state *mrb, mrb_int idx)
{
  struct RProc *p = mrb->c->ci->proc;
  struct REnv *e;

  if (!p || !MRB_PROC_CFUNC_P(p)) {
    mrb_raise(mrb, E_TYPE_ERROR, "Can't get cfunc env from non-cfunc proc.");
  }
  e = MRB_PROC_ENV(p);
  if (!e) {
    mrb_raise(mrb, E_TYPE_ERROR, "Can't get cfunc env from cfunc Proc without REnv.");
  }
  if (idx < 0 || MRB_ENV_STACK_LEN(e) <= idx) {
    mrb_raisef(mrb, E_INDEX_ERROR, "Env index out of range: %i (expected: 0 <= index < %i)",
               idx, MRB_ENV_STACK_LEN(e));
  }

  return e->stack[idx];
}

void
mrb_proc_copy(struct RProc *a, struct RProc *b)
{
  if (a->body.irep) {
    /* already initialized proc */
    return;
  }
  a->flags = b->flags;
  a->body = b->body;
  if (!MRB_PROC_CFUNC_P(a) && a->body.irep) {
    a->body.irep->refcnt++;
  }
  a->upper = b->upper;
  a->e.env = b->e.env;
  /* a->e.target_class = a->e.target_class; */
}

static mrb_value
mrb_proc_s_new(mrb_state *mrb, mrb_value proc_class)
{
  mrb_value blk;
  mrb_value proc;
  struct RProc *p;

  /* Calling Proc.new without a block is not implemented yet */
  mrb_get_args(mrb, "&!", &blk);
  p = (struct RProc *)mrb_obj_alloc(mrb, MRB_TT_PROC, mrb_class_ptr(proc_class));
  mrb_proc_copy(p, mrb_proc_ptr(blk));
  proc = mrb_obj_value(p);
  mrb_funcall_with_block(mrb, proc, mrb_intern_lit(mrb, "initialize"), 0, NULL, proc);
  if (!MRB_PROC_STRICT_P(p) &&
      mrb->c->ci > mrb->c->cibase && MRB_PROC_ENV(p) == mrb->c->ci[-1].env) {
    p->flags |= MRB_PROC_ORPHAN;
  }
  return proc;
}

static mrb_value
mrb_proc_init_copy(mrb_state *mrb, mrb_value self)
{
  mrb_value proc = mrb_get_arg1(mrb);

  if (!mrb_proc_p(proc)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "not a proc");
  }
  mrb_proc_copy(mrb_proc_ptr(self), mrb_proc_ptr(proc));
  return self;
}

/* 15.2.17.4.2 */
static mrb_value
proc_arity(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(mrb_proc_arity(mrb_proc_ptr(self)));
}

/* 15.3.1.2.6  */
/* 15.3.1.3.27 */
/*
 * call-seq:
 *   lambda { |...| block }  -> a_proc
 *
 * Equivalent to <code>Proc.new</code>, except the resulting Proc objects
 * check the number of parameters passed when called.
 */
static mrb_value
proc_lambda(mrb_state *mrb, mrb_value self)
{
  mrb_value blk;
  struct RProc *p;

  mrb_get_args(mrb, "&", &blk);
  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "tried to create Proc object without a block");
  }
  if (!mrb_proc_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "not a proc");
  }
  p = mrb_proc_ptr(blk);
  if (!MRB_PROC_STRICT_P(p)) {
    struct RProc *p2 = (struct RProc*)mrb_obj_alloc(mrb, MRB_TT_PROC, p->c);
    mrb_proc_copy(p2, p);
    p2->flags |= MRB_PROC_STRICT;
    return mrb_obj_value(p2);
  }
  return blk;
}

mrb_int
mrb_proc_arity(const struct RProc *p)
{
  struct mrb_irep *irep;
  const mrb_code *pc;
  mrb_aspec aspec;
  int ma, op, ra, pa, arity;

  if (MRB_PROC_CFUNC_P(p)) {
    /* TODO cfunc aspec not implemented yet */
    return -1;
  }

  irep = p->body.irep;
  if (!irep) {
    return 0;
  }

  pc = irep->iseq;
  /* arity is depend on OP_ENTER */
  if (*pc != OP_ENTER) {
    return 0;
  }

  aspec = PEEK_W(pc+1);
  ma = MRB_ASPEC_REQ(aspec);
  op = MRB_ASPEC_OPT(aspec);
  ra = MRB_ASPEC_REST(aspec);
  pa = MRB_ASPEC_POST(aspec);
  arity = ra || (MRB_PROC_STRICT_P(p) && op) ? -(ma + pa + 1) : ma + pa;

  return arity;
}

static void
tempirep_free(mrb_state *mrb, void *p)
{
  if (p) mrb_irep_free(mrb, (mrb_irep *)p);
}

static const mrb_data_type tempirep_type = { "temporary irep", tempirep_free };

void
mrb_init_proc(mrb_state *mrb)
{
  struct RProc *p;
  mrb_method_t m;
  struct RData *irep_obj = mrb_data_object_alloc(mrb, mrb->object_class, NULL, &tempirep_type);
  mrb_irep *call_irep;
  static const mrb_irep mrb_irep_zero = { 0 };

  call_irep = (mrb_irep *)mrb_malloc(mrb, sizeof(mrb_irep));
  irep_obj->data = call_irep;
  *call_irep = mrb_irep_zero;
  call_irep->flags = MRB_ISEQ_NO_FREE;
  call_irep->iseq = call_iseq;
  call_irep->ilen = 1;
  call_irep->nregs = 2;         /* receiver and block */

  mrb_define_class_method(mrb, mrb->proc_class, "new", mrb_proc_s_new, MRB_ARGS_NONE()|MRB_ARGS_BLOCK());
  mrb_define_method(mrb, mrb->proc_class, "initialize_copy", mrb_proc_init_copy, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, mrb->proc_class, "arity", proc_arity, MRB_ARGS_NONE());

  p = mrb_proc_new(mrb, call_irep);
  irep_obj->data = NULL;
  MRB_METHOD_FROM_PROC(m, p);
  mrb_define_method_raw(mrb, mrb->proc_class, mrb_intern_lit(mrb, "call"), m);
  mrb_define_method_raw(mrb, mrb->proc_class, mrb_intern_lit(mrb, "[]"), m);

  mrb_define_class_method(mrb, mrb->kernel_module, "lambda", proc_lambda, MRB_ARGS_NONE()|MRB_ARGS_BLOCK()); /* 15.3.1.2.6  */
  mrb_define_method(mrb, mrb->kernel_module,       "lambda", proc_lambda, MRB_ARGS_NONE()|MRB_ARGS_BLOCK()); /* 15.3.1.3.27 */
}
/*
** range.c - Range class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/range.h>
#include <mruby/string.h>
#include <mruby/array.h>

#define RANGE_INITIALIZED_MASK 1
#define RANGE_INITIALIZED(p) ((p)->flags |= RANGE_INITIALIZED_MASK)
#define RANGE_INITIALIZED_P(p) ((p)->flags & RANGE_INITIALIZED_MASK)

static void
r_check(mrb_state *mrb, mrb_value a, mrb_value b)
{
  enum mrb_vtype ta;
  enum mrb_vtype tb;
  mrb_int n;

  ta = mrb_type(a);
  tb = mrb_type(b);
#ifdef MRB_WITHOUT_FLOAT
  if (ta == MRB_TT_FIXNUM && tb == MRB_TT_FIXNUM ) {
#else
  if ((ta == MRB_TT_FIXNUM || ta == MRB_TT_FLOAT) &&
      (tb == MRB_TT_FIXNUM || tb == MRB_TT_FLOAT)) {
#endif
    return;
  }

  n = mrb_cmp(mrb, a, b);
  if (n == -2) {                /* can not be compared */
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bad value for range");
  }
}

static mrb_bool
r_le(mrb_state *mrb, mrb_value a, mrb_value b)
{
  mrb_int n = mrb_cmp(mrb, a, b);

  if (n == 0 || n == -1) return TRUE;
  return FALSE;
}

static mrb_bool
r_gt(mrb_state *mrb, mrb_value a, mrb_value b)
{
  return mrb_cmp(mrb, a, b) == 1;
}

static mrb_bool
r_ge(mrb_state *mrb, mrb_value a, mrb_value b)
{
  mrb_int n = mrb_cmp(mrb, a, b);

  if (n == 0 || n == 1) return TRUE;
  return FALSE;
}

static void
range_ptr_alloc_edges(mrb_state *mrb, struct RRange *r)
{
#ifndef MRB_RANGE_EMBED
  r->edges = (mrb_range_edges *)mrb_malloc(mrb, sizeof(mrb_range_edges));
#endif
}

static struct RRange *
range_ptr_init(mrb_state *mrb, struct RRange *r, mrb_value beg, mrb_value end, mrb_bool excl)
{
  r_check(mrb, beg, end);

  if (r) {
    if (RANGE_INITIALIZED_P(r)) {
      /* Ranges are immutable, so that they should be initialized only once. */
      mrb_name_error(mrb, mrb_intern_lit(mrb, "initialize"), "'initialize' called twice");
    }
    else {
      range_ptr_alloc_edges(mrb, r);
    }
  }
  else {
    r = (struct RRange*)mrb_obj_alloc(mrb, MRB_TT_RANGE, mrb->range_class);
    range_ptr_alloc_edges(mrb, r);
  }

  RANGE_BEG(r) = beg;
  RANGE_END(r) = end;
  RANGE_EXCL(r) = excl;
  RANGE_INITIALIZED(r);

  return r;
}

static void
range_ptr_replace(mrb_state *mrb, struct RRange *r, mrb_value beg, mrb_value end, mrb_bool excl)
{
  range_ptr_init(mrb, r, beg, end, excl);
  mrb_write_barrier(mrb, (struct RBasic*)r);
}

/*
 *  call-seq:
 *     rng.first    => obj
 *     rng.begin    => obj
 *
 *  Returns the first object in <i>rng</i>.
 */
static mrb_value
range_beg(mrb_state *mrb, mrb_value range)
{
  return mrb_range_beg(mrb, range);
}

/*
 *  call-seq:
 *     rng.end    => obj
 *     rng.last   => obj
 *
 *  Returns the object that defines the end of <i>rng</i>.
 *
 *     (1..10).end    #=> 10
 *     (1...10).end   #=> 10
 */
static mrb_value
range_end(mrb_state *mrb, mrb_value range)
{
  return mrb_range_end(mrb, range);
}

/*
 *  call-seq:
 *     range.exclude_end?    => true or false
 *
 *  Returns <code>true</code> if <i>range</i> excludes its end value.
 */
static mrb_value
range_excl(mrb_state *mrb, mrb_value range)
{
  return mrb_bool_value(mrb_range_excl_p(mrb, range));
}

/*
 *  call-seq:
 *     Range.new(start, end, exclusive=false)    => range
 *
 *  Constructs a range using the given <i>start</i> and <i>end</i>. If the third
 *  parameter is omitted or is <code>false</code>, the <i>range</i> will include
 *  the end object; otherwise, it will be excluded.
 */
static mrb_value
range_initialize(mrb_state *mrb, mrb_value range)
{
  mrb_value beg, end;
  mrb_bool exclusive = FALSE;

  mrb_get_args(mrb, "oo|b", &beg, &end, &exclusive);
  range_ptr_replace(mrb, mrb_range_raw_ptr(range), beg, end, exclusive);
  return range;
}

/*
 *  call-seq:
 *     range == obj    => true or false
 *
 *  Returns <code>true</code> only if
 *  1) <i>obj</i> is a Range,
 *  2) <i>obj</i> has equivalent beginning and end items (by comparing them with <code>==</code>),
 *  3) <i>obj</i> has the same #exclude_end? setting as <i>rng</t>.
 *
 *    (0..2) == (0..2)            #=> true
 *    (0..2) == Range.new(0,2)    #=> true
 *    (0..2) == (0...2)           #=> false
 */
static mrb_value
range_eq(mrb_state *mrb, mrb_value range)
{
  struct RRange *rr;
  struct RRange *ro;
  mrb_value obj = mrb_get_arg1(mrb);
  mrb_bool v1, v2;

  if (mrb_obj_equal(mrb, range, obj)) return mrb_true_value();
  if (!mrb_obj_is_instance_of(mrb, obj, mrb_obj_class(mrb, range))) { /* same class? */
    return mrb_false_value();
  }

  rr = mrb_range_ptr(mrb, range);
  ro = mrb_range_ptr(mrb, obj);
  v1 = mrb_equal(mrb, RANGE_BEG(rr), RANGE_BEG(ro));
  v2 = mrb_equal(mrb, RANGE_END(rr), RANGE_END(ro));
  if (!v1 || !v2 || RANGE_EXCL(rr) != RANGE_EXCL(ro)) {
    return mrb_false_value();
  }
  return mrb_true_value();
}

/*
 *  call-seq:
 *     range === obj       =>  true or false
 *     range.member?(val)  =>  true or false
 *     range.include?(val) =>  true or false
 */
static mrb_value
range_include(mrb_state *mrb, mrb_value range)
{
  mrb_value val = mrb_get_arg1(mrb);
  struct RRange *r = mrb_range_ptr(mrb, range);
  mrb_value beg, end;
  mrb_bool include_p;

  beg = RANGE_BEG(r);
  end = RANGE_END(r);
  include_p = r_le(mrb, beg, val) &&                 /* beg <= val */
              (RANGE_EXCL(r) ? r_gt(mrb, end, val)   /* end >  val */
                             : r_ge(mrb, end, val)); /* end >= val */

  return mrb_bool_value(include_p);
}

/* 15.2.14.4.12(x) */
/*
 * call-seq:
 *   rng.to_s   -> string
 *
 * Convert this range object to a printable form.
 */
static mrb_value
range_to_s(mrb_state *mrb, mrb_value range)
{
  mrb_value str, str2;
  struct RRange *r = mrb_range_ptr(mrb, range);

  str  = mrb_obj_as_string(mrb, RANGE_BEG(r));
  str2 = mrb_obj_as_string(mrb, RANGE_END(r));
  str  = mrb_str_dup(mrb, str);
  mrb_str_cat(mrb, str, "...", RANGE_EXCL(r) ? 3 : 2);
  mrb_str_cat_str(mrb, str, str2);

  return str;
}

/* 15.2.14.4.13(x) */
/*
 * call-seq:
 *   rng.inspect  -> string
 *
 * Convert this range object to a printable form (using
 * <code>inspect</code> to convert the start and end
 * objects).
 */
static mrb_value
range_inspect(mrb_state *mrb, mrb_value range)
{
  mrb_value str, str2;
  struct RRange *r = mrb_range_ptr(mrb, range);

  str  = mrb_inspect(mrb, RANGE_BEG(r));
  str2 = mrb_inspect(mrb, RANGE_END(r));
  str  = mrb_str_dup(mrb, str);
  mrb_str_cat(mrb, str, "...", RANGE_EXCL(r) ? 3 : 2);
  mrb_str_cat_str(mrb, str, str2);

  return str;
}

/* 15.2.14.4.14(x) */
/*
 *  call-seq:
 *     rng.eql?(obj)    -> true or false
 *
 *  Returns <code>true</code> only if <i>obj</i> is a Range, has equivalent
 *  beginning and end items (by comparing them with #eql?), and has the same
 *  #exclude_end? setting as <i>rng</i>.
 *
 *    (0..2).eql?(0..2)            #=> true
 *    (0..2).eql?(Range.new(0,2))  #=> true
 *    (0..2).eql?(0...2)           #=> false
 */
static mrb_value
range_eql(mrb_state *mrb, mrb_value range)
{
  mrb_value obj = mrb_get_arg1(mrb);
  struct RRange *r, *o;

  if (mrb_obj_equal(mrb, range, obj)) return mrb_true_value();
  if (!mrb_obj_is_kind_of(mrb, obj, mrb->range_class)) return mrb_false_value();
  if (!mrb_range_p(obj)) return mrb_false_value();

  r = mrb_range_ptr(mrb, range);
  o = mrb_range_ptr(mrb, obj);
  if (!mrb_eql(mrb, RANGE_BEG(r), RANGE_BEG(o)) ||
      !mrb_eql(mrb, RANGE_END(r), RANGE_END(o)) ||
      (RANGE_EXCL(r) != RANGE_EXCL(o))) {
    return mrb_false_value();
  }
  return mrb_true_value();
}

/* 15.2.14.4.15(x) */
static mrb_value
range_initialize_copy(mrb_state *mrb, mrb_value copy)
{
  mrb_value src = mrb_get_arg1(mrb);
  struct RRange *r;

  if (mrb_obj_equal(mrb, copy, src)) return copy;
  if (!mrb_obj_is_instance_of(mrb, src, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }

  r = mrb_range_ptr(mrb, src);
  range_ptr_replace(mrb, mrb_range_raw_ptr(copy), RANGE_BEG(r), RANGE_END(r), RANGE_EXCL(r));

  return copy;
}

mrb_value
mrb_get_values_at(mrb_state *mrb, mrb_value obj, mrb_int olen, mrb_int argc, const mrb_value *argv, mrb_value (*func)(mrb_state*, mrb_value, mrb_int))
{
  mrb_int i, j, beg, len;
  mrb_value result;
  result = mrb_ary_new(mrb);

  for (i = 0; i < argc; ++i) {
    if (mrb_fixnum_p(argv[i])) {
      mrb_ary_push(mrb, result, func(mrb, obj, mrb_fixnum(argv[i])));
    }
    else if (mrb_range_beg_len(mrb, argv[i], &beg, &len, olen, FALSE) == MRB_RANGE_OK) {
      mrb_int const end = olen < beg + len ? olen : beg + len;
      for (j = beg; j < end; ++j) {
        mrb_ary_push(mrb, result, func(mrb, obj, j));
      }

      for (; j < beg + len; ++j) {
        mrb_ary_push(mrb, result, mrb_nil_value());
      }
    }
    else {
      mrb_raisef(mrb, E_TYPE_ERROR, "invalid values selector: %v", argv[i]);
    }
  }

  return result;
}

void
mrb_gc_mark_range(mrb_state *mrb, struct RRange *r)
{
  if (RANGE_INITIALIZED_P(r)) {
    mrb_gc_mark_value(mrb, RANGE_BEG(r));
    mrb_gc_mark_value(mrb, RANGE_END(r));
  }
}

MRB_API struct RRange*
mrb_range_ptr(mrb_state *mrb, mrb_value range)
{
  struct RRange *r = mrb_range_raw_ptr(range);

  /* check for if #initialize_copy was removed [#3320] */
  if (!RANGE_INITIALIZED_P(r)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "uninitialized range");
  }
  return r;
}

MRB_API mrb_value
mrb_range_new(mrb_state *mrb, mrb_value beg, mrb_value end, mrb_bool excl)
{
  struct RRange *r = range_ptr_init(mrb, NULL, beg, end, excl);
  return mrb_range_value(r);
}

MRB_API enum mrb_range_beg_len
mrb_range_beg_len(mrb_state *mrb, mrb_value range, mrb_int *begp, mrb_int *lenp, mrb_int len, mrb_bool trunc)
{
  mrb_int beg, end;
  struct RRange *r;

  if (!mrb_range_p(range)) return MRB_RANGE_TYPE_MISMATCH;
  r = mrb_range_ptr(mrb, range);

  beg = mrb_int(mrb, RANGE_BEG(r));
  end = mrb_int(mrb, RANGE_END(r));

  if (beg < 0) {
    beg += len;
    if (beg < 0) return MRB_RANGE_OUT;
  }

  if (trunc) {
    if (beg > len) return MRB_RANGE_OUT;
    if (end > len) end = len;
  }

  if (end < 0) end += len;
  if (!RANGE_EXCL(r) && (!trunc || end < len)) end++;  /* include end point */
  len = end - beg;
  if (len < 0) len = 0;

  *begp = beg;
  *lenp = len;
  return MRB_RANGE_OK;
}

void
mrb_init_range(mrb_state *mrb)
{
  struct RClass *r;

  r = mrb_define_class(mrb, "Range", mrb->object_class);                                /* 15.2.14 */
  mrb->range_class = r;
  MRB_SET_INSTANCE_TT(r, MRB_TT_RANGE);

  mrb_define_method(mrb, r, "begin",           range_beg,             MRB_ARGS_NONE()); /* 15.2.14.4.3  */
  mrb_define_method(mrb, r, "end",             range_end,             MRB_ARGS_NONE()); /* 15.2.14.4.5  */
  mrb_define_method(mrb, r, "==",              range_eq,              MRB_ARGS_REQ(1)); /* 15.2.14.4.1  */
  mrb_define_method(mrb, r, "===",             range_include,         MRB_ARGS_REQ(1)); /* 15.2.14.4.2  */
  mrb_define_method(mrb, r, "exclude_end?",    range_excl,            MRB_ARGS_NONE()); /* 15.2.14.4.6  */
  mrb_define_method(mrb, r, "first",           range_beg,             MRB_ARGS_NONE()); /* 15.2.14.4.7  */
  mrb_define_method(mrb, r, "include?",        range_include,         MRB_ARGS_REQ(1)); /* 15.2.14.4.8  */
  mrb_define_method(mrb, r, "initialize",      range_initialize,      MRB_ARGS_ANY());  /* 15.2.14.4.9  */
  mrb_define_method(mrb, r, "last",            range_end,             MRB_ARGS_NONE()); /* 15.2.14.4.10 */
  mrb_define_method(mrb, r, "member?",         range_include,         MRB_ARGS_REQ(1)); /* 15.2.14.4.11 */
  mrb_define_method(mrb, r, "to_s",            range_to_s,            MRB_ARGS_NONE()); /* 15.2.14.4.12(x) */
  mrb_define_method(mrb, r, "inspect",         range_inspect,         MRB_ARGS_NONE()); /* 15.2.14.4.13(x) */
  mrb_define_method(mrb, r, "eql?",            range_eql,             MRB_ARGS_REQ(1)); /* 15.2.14.4.14(x) */
  mrb_define_method(mrb, r, "initialize_copy", range_initialize_copy, MRB_ARGS_REQ(1)); /* 15.2.14.4.15(x) */
}
/*
** state.c - mrb_state open/close functions
**
** See Copyright Notice in mruby.h
*/

#include <stdlib.h>
#include <string.h>
#include <mruby.h>
#include <mruby/irep.h>
#include <mruby/variable.h>
#include <mruby/debug.h>
#include <mruby/string.h>
#include <mruby/class.h>

void mrb_init_core(mrb_state*);
void mrb_init_mrbgems(mrb_state*);

void mrb_gc_init(mrb_state*, mrb_gc *gc);
void mrb_gc_destroy(mrb_state*, mrb_gc *gc);

int mrb_core_init_protect(mrb_state *mrb, void (*body)(mrb_state *, void *), void *opaque);

static void
init_gc_and_core(mrb_state *mrb, void *opaque)
{
  static const struct mrb_context mrb_context_zero = { 0 };

  mrb_gc_init(mrb, &mrb->gc);
  mrb->c = (struct mrb_context*)mrb_malloc(mrb, sizeof(struct mrb_context));
  *mrb->c = mrb_context_zero;
  mrb->root_c = mrb->c;

  mrb_init_core(mrb);
}

MRB_API mrb_state*
mrb_open_core(mrb_allocf f, void *ud)
{
  static const mrb_state mrb_state_zero = { 0 };
  mrb_state *mrb;

  if (f == NULL) f = mrb_default_allocf;
  mrb = (mrb_state *)(f)(NULL, NULL, sizeof(mrb_state), ud);
  if (mrb == NULL) return NULL;

  *mrb = mrb_state_zero;
  mrb->allocf_ud = ud;
  mrb->allocf = f;
  mrb->atexit_stack_len = 0;

  if (mrb_core_init_protect(mrb, init_gc_and_core, NULL)) {
    mrb_close(mrb);
    return NULL;
  }

  return mrb;
}

void*
mrb_default_allocf(mrb_state *mrb, void *p, size_t size, void *ud)
{
  if (size == 0) {
    free(p);
    return NULL;
  }
  else {
    return realloc(p, size);
  }
}

MRB_API mrb_state*
mrb_open(void)
{
  mrb_state *mrb = mrb_open_allocf(mrb_default_allocf, NULL);

  return mrb;
}

static void
init_mrbgems(mrb_state *mrb, void *opaque)
{
  mrb_init_mrbgems(mrb);
}

MRB_API mrb_state*
mrb_open_allocf(mrb_allocf f, void *ud)
{
  mrb_state *mrb = mrb_open_core(f, ud);

  if (mrb == NULL) {
    return NULL;
  }

#ifndef DISABLE_GEMS
  if (mrb_core_init_protect(mrb, init_mrbgems, NULL)) {
    mrb_close(mrb);
    return NULL;
  }
  mrb_gc_arena_restore(mrb, 0);
#endif
  return mrb;
}

void mrb_free_symtbl(mrb_state *mrb);

void
mrb_irep_incref(mrb_state *mrb, mrb_irep *irep)
{
  irep->refcnt++;
}

void
mrb_irep_decref(mrb_state *mrb, mrb_irep *irep)
{
  irep->refcnt--;
  if (irep->refcnt == 0) {
    mrb_irep_free(mrb, irep);
  }
}

void
mrb_irep_cutref(mrb_state *mrb, mrb_irep *irep)
{
  mrb_irep *tmp;
  int i;

  for (i=0; i<irep->rlen; i++) {
    tmp = irep->reps[i];
    irep->reps[i] = NULL;
    if (tmp) mrb_irep_decref(mrb, tmp);
  }
}

void
mrb_irep_free(mrb_state *mrb, mrb_irep *irep)
{
  int i;

  if (!(irep->flags & MRB_ISEQ_NO_FREE))
    mrb_free(mrb, (void*)irep->iseq);
  if (irep->pool) for (i=0; i<irep->plen; i++) {
    if (mrb_string_p(irep->pool[i])) {
      mrb_gc_free_str(mrb, RSTRING(irep->pool[i]));
      mrb_free(mrb, mrb_obj_ptr(irep->pool[i]));
    }
#if defined(MRB_WORD_BOXING) && !defined(MRB_WITHOUT_FLOAT)
    else if (mrb_float_p(irep->pool[i])) {
      mrb_free(mrb, mrb_obj_ptr(irep->pool[i]));
    }
#endif
  }
  mrb_free(mrb, irep->pool);
  mrb_free(mrb, irep->syms);
  if (irep->reps) {
    for (i=0; i<irep->rlen; i++) {
      if (irep->reps[i])
        mrb_irep_decref(mrb, irep->reps[i]);
    }
  }
  mrb_free(mrb, irep->reps);
  mrb_free(mrb, irep->lv);
  mrb_debug_info_free(mrb, irep->debug_info);
  mrb_free(mrb, irep);
}

void mrb_free_backtrace(mrb_state *mrb);

MRB_API void
mrb_free_context(mrb_state *mrb, struct mrb_context *c)
{
  if (!c) return;
  mrb_free(mrb, c->stbase);
  mrb_free(mrb, c->cibase);
  mrb_free(mrb, c->rescue);
  mrb_free(mrb, c->ensure);
  mrb_free(mrb, c);
}

MRB_API void
mrb_close(mrb_state *mrb)
{
  if (!mrb) return;
  if (mrb->atexit_stack_len > 0) {
    mrb_int i;
    for (i = mrb->atexit_stack_len; i > 0; --i) {
      mrb->atexit_stack[i - 1](mrb);
    }
#ifndef MRB_FIXED_STATE_ATEXIT_STACK
    mrb_free(mrb, mrb->atexit_stack);
#endif
  }

  /* free */
  mrb_gc_destroy(mrb, &mrb->gc);
  mrb_free_context(mrb, mrb->root_c);
  mrb_gc_free_gv(mrb);
  mrb_free_symtbl(mrb);
  mrb_free(mrb, mrb);
}

MRB_API mrb_irep*
mrb_add_irep(mrb_state *mrb)
{
  static const mrb_irep mrb_irep_zero = { 0 };
  mrb_irep *irep;

  irep = (mrb_irep *)mrb_malloc(mrb, sizeof(mrb_irep));
  *irep = mrb_irep_zero;
  irep->refcnt = 1;

  return irep;
}

MRB_API mrb_value
mrb_top_self(mrb_state *mrb)
{
  return mrb_obj_value(mrb->top_self);
}

MRB_API void
mrb_state_atexit(mrb_state *mrb, mrb_atexit_func f)
{
#ifdef MRB_FIXED_STATE_ATEXIT_STACK
  if (mrb->atexit_stack_len + 1 > MRB_FIXED_STATE_ATEXIT_STACK_SIZE) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "exceeded fixed state atexit stack limit");
  }
#else
  size_t stack_size;

  stack_size = sizeof(mrb_atexit_func) * (mrb->atexit_stack_len + 1);
  if (mrb->atexit_stack_len == 0) {
    mrb->atexit_stack = (mrb_atexit_func*)mrb_malloc(mrb, stack_size);
  }
  else {
    mrb->atexit_stack = (mrb_atexit_func*)mrb_realloc(mrb, mrb->atexit_stack, stack_size);
  }
#endif

  mrb->atexit_stack[mrb->atexit_stack_len++] = f;
}
/*
** string.c - String class
**
** See Copyright Notice in mruby.h
*/

#ifdef _MSC_VER
# define _CRT_NONSTDC_NO_DEPRECATE
#endif

#ifndef MRB_WITHOUT_FLOAT
#include <float.h>
#include <math.h>
#endif
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/range.h>
#include <mruby/string.h>
#include <mruby/numeric.h>

typedef struct mrb_shared_string {
  int refcnt;
  mrb_ssize capa;
  char *ptr;
} mrb_shared_string;

const char mrb_digitmap[] = "0123456789abcdefghijklmnopqrstuvwxyz";

#define mrb_obj_alloc_string(mrb) ((struct RString*)mrb_obj_alloc((mrb), MRB_TT_STRING, (mrb)->string_class))

static struct RString*
str_init_normal_capa(mrb_state *mrb, struct RString *s,
                     const char *p, size_t len, size_t capa)
{
  char *dst = (char *)mrb_malloc(mrb, capa + 1);
  if (p) memcpy(dst, p, len);
  dst[len] = '\0';
  s->as.heap.ptr = dst;
  s->as.heap.len = (mrb_ssize)len;
  s->as.heap.aux.capa = (mrb_ssize)capa;
  RSTR_UNSET_TYPE_FLAG(s);
  return s;
}

static struct RString*
str_init_normal(mrb_state *mrb, struct RString *s, const char *p, size_t len)
{
  return str_init_normal_capa(mrb, s, p, len, len);
}

static struct RString*
str_init_embed(struct RString *s, const char *p, size_t len)
{
  if (p) memcpy(RSTR_EMBED_PTR(s), p, len);
  RSTR_EMBED_PTR(s)[len] = '\0';
  RSTR_SET_TYPE_FLAG(s, EMBED);
  RSTR_SET_EMBED_LEN(s, len);
  return s;
}

static struct RString*
str_init_nofree(struct RString *s, const char *p, size_t len)
{
  s->as.heap.ptr = (char *)p;
  s->as.heap.len = (mrb_ssize)len;
  s->as.heap.aux.capa = 0;             /* nofree */
  RSTR_SET_TYPE_FLAG(s, NOFREE);
  return s;
}

static struct RString*
str_init_shared(mrb_state *mrb, const struct RString *orig, struct RString *s, mrb_shared_string *shared)
{
  if (shared) {
    shared->refcnt++;
  }
  else {
    shared = (mrb_shared_string *)mrb_malloc(mrb, sizeof(mrb_shared_string));
    shared->refcnt = 1;
    shared->ptr = orig->as.heap.ptr;
    shared->capa = orig->as.heap.aux.capa;
  }
  s->as.heap.ptr = orig->as.heap.ptr;
  s->as.heap.len = orig->as.heap.len;
  s->as.heap.aux.shared = shared;
  RSTR_SET_TYPE_FLAG(s, SHARED);
  return s;
}

static struct RString*
str_init_fshared(const struct RString *orig, struct RString *s, struct RString *fshared)
{
  s->as.heap.ptr = orig->as.heap.ptr;
  s->as.heap.len = orig->as.heap.len;
  s->as.heap.aux.fshared = fshared;
  RSTR_SET_TYPE_FLAG(s, FSHARED);
  return s;
}

static struct RString*
str_init_modifiable(mrb_state *mrb, struct RString *s, const char *p, size_t len)
{
  if (RSTR_EMBEDDABLE_P(len)) {
    return str_init_embed(s, p, len);
  }
  else {
    return str_init_normal(mrb, s, p, len);
  }
}

static struct RString*
str_new_static(mrb_state *mrb, const char *p, size_t len)
{
  if (RSTR_EMBEDDABLE_P(len)) {
    return str_init_embed(mrb_obj_alloc_string(mrb), p, len);
  }
  if (len >= MRB_SSIZE_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string size too big");
  }
  return str_init_nofree(mrb_obj_alloc_string(mrb), p, len);
}

static struct RString*
str_new(mrb_state *mrb, const char *p, size_t len)
{
  if (RSTR_EMBEDDABLE_P(len)) {
    return str_init_embed(mrb_obj_alloc_string(mrb), p, len);
  }
  if (len >= MRB_SSIZE_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string size too big");
  }
  if (p && mrb_ro_data_p(p)) {
    return str_init_nofree(mrb_obj_alloc_string(mrb), p, len);
  }
  return str_init_normal(mrb, mrb_obj_alloc_string(mrb), p, len);
}

static inline void
str_with_class(struct RString *s, mrb_value obj)
{
  s->c = mrb_str_ptr(obj)->c;
}

static mrb_value
mrb_str_new_empty(mrb_state *mrb, mrb_value str)
{
  struct RString *s = str_new(mrb, 0, 0);

  str_with_class(s, str);
  return mrb_obj_value(s);
}

MRB_API mrb_value
mrb_str_new_capa(mrb_state *mrb, size_t capa)
{
  struct RString *s;

  if (RSTR_EMBEDDABLE_P(capa)) {
    s = str_init_embed(mrb_obj_alloc_string(mrb), NULL, 0);
  }
  else if (capa >= MRB_SSIZE_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string capacity size too big");
    /* not reached */
    s = NULL;
  }
  else {
    s = str_init_normal_capa(mrb, mrb_obj_alloc_string(mrb), NULL, 0, capa);
  }

  return mrb_obj_value(s);
}

#ifndef MRB_STR_BUF_MIN_SIZE
# define MRB_STR_BUF_MIN_SIZE 128
#endif

MRB_API mrb_value
mrb_str_buf_new(mrb_state *mrb, size_t capa)
{
  if (capa < MRB_STR_BUF_MIN_SIZE) {
    capa = MRB_STR_BUF_MIN_SIZE;
  }
  return mrb_str_new_capa(mrb, capa);
}

static void
resize_capa(mrb_state *mrb, struct RString *s, size_t capacity)
{
#if SIZE_MAX > MRB_SSIZE_MAX
    mrb_assert(capacity < MRB_SSIZE_MAX);
#endif
  if (RSTR_EMBED_P(s)) {
    if (!RSTR_EMBEDDABLE_P(capacity)) {
      str_init_normal_capa(mrb, s, RSTR_EMBED_PTR(s), RSTR_EMBED_LEN(s), capacity);
    }
  }
  else {
    s->as.heap.ptr = (char*)mrb_realloc(mrb, RSTR_PTR(s), capacity+1);
    s->as.heap.aux.capa = (mrb_ssize)capacity;
  }
}

MRB_API mrb_value
mrb_str_new(mrb_state *mrb, const char *p, size_t len)
{
  return mrb_obj_value(str_new(mrb, p, len));
}

MRB_API mrb_value
mrb_str_new_cstr(mrb_state *mrb, const char *p)
{
  struct RString *s;
  size_t len;

  if (p) {
    len = strlen(p);
  }
  else {
    len = 0;
  }

  s = str_new(mrb, p, len);

  return mrb_obj_value(s);
}

MRB_API mrb_value
mrb_str_new_static(mrb_state *mrb, const char *p, size_t len)
{
  struct RString *s = str_new_static(mrb, p, len);
  return mrb_obj_value(s);
}

static void
str_decref(mrb_state *mrb, mrb_shared_string *shared)
{
  shared->refcnt--;
  if (shared->refcnt == 0) {
    mrb_free(mrb, shared->ptr);
    mrb_free(mrb, shared);
  }
}

static void
str_modify_keep_ascii(mrb_state *mrb, struct RString *s)
{
  if (RSTR_SHARED_P(s)) {
    mrb_shared_string *shared = s->as.heap.aux.shared;

    if (shared->refcnt == 1 && s->as.heap.ptr == shared->ptr) {
      s->as.heap.aux.capa = shared->capa;
      s->as.heap.ptr[s->as.heap.len] = '\0';
      RSTR_UNSET_SHARED_FLAG(s);
      mrb_free(mrb, shared);
    }
    else {
      str_init_modifiable(mrb, s, s->as.heap.ptr, (size_t)s->as.heap.len);
      str_decref(mrb, shared);
    }
  }
  else if (RSTR_NOFREE_P(s) || RSTR_FSHARED_P(s)) {
    str_init_modifiable(mrb, s, s->as.heap.ptr, (size_t)s->as.heap.len);
  }
}

static void
check_null_byte(mrb_state *mrb, mrb_value str)
{
  mrb_to_str(mrb, str);
  if (memchr(RSTRING_PTR(str), '\0', RSTRING_LEN(str))) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string contains null byte");
  }
}

void
mrb_gc_free_str(mrb_state *mrb, struct RString *str)
{
  if (RSTR_EMBED_P(str))
    /* no code */;
  else if (RSTR_SHARED_P(str))
    str_decref(mrb, str->as.heap.aux.shared);
  else if (!RSTR_NOFREE_P(str) && !RSTR_FSHARED_P(str))
    mrb_free(mrb, str->as.heap.ptr);
}

#ifdef MRB_UTF8_STRING
static const char utf8len_codepage[256] =
{
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,1,1,1,1,1,1,1,1,1,1,1,
};

mrb_int
mrb_utf8len(const char* p, const char* e)
{
  mrb_int len;
  mrb_int i;

  if ((unsigned char)*p < 0x80) return 1;
  len = utf8len_codepage[(unsigned char)*p];
  if (len == 1) return 1;
  if (len > e - p) return 1;
  for (i = 1; i < len; ++i)
    if ((p[i] & 0xc0) != 0x80)
      return 1;
  return len;
}

mrb_int
mrb_utf8_strlen(const char *str, mrb_int byte_len)
{
  mrb_int total = 0;
  const char *p = str;
  const char *e = p + byte_len;

  while (p < e) {
    p += mrb_utf8len(p, e);
    total++;
  }
  return total;
}

static mrb_int
utf8_strlen(mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  mrb_int byte_len = RSTR_LEN(s);

  if (RSTR_ASCII_P(s)) {
    return byte_len;
  }
  else {
    mrb_int utf8_len = mrb_utf8_strlen(RSTR_PTR(s), byte_len);
    if (byte_len == utf8_len) RSTR_SET_ASCII_FLAG(s);
    return utf8_len;
  }
}

#define RSTRING_CHAR_LEN(s) utf8_strlen(s)

/* map character index to byte offset index */
static mrb_int
chars2bytes(mrb_value s, mrb_int off, mrb_int idx)
{
  if (RSTR_ASCII_P(mrb_str_ptr(s))) {
    return idx;
  }
  else {
    mrb_int i, b, n;
    const char *p = RSTRING_PTR(s) + off;
    const char *e = RSTRING_END(s);

    for (b=i=0; p<e && i<idx; i++) {
      n = mrb_utf8len(p, e);
      b += n;
      p += n;
    }
    return b;
  }
}

/* map byte offset to character index */
static mrb_int
bytes2chars(char *p, mrb_int len, mrb_int bi)
{
  const char *e = p + (size_t)len;
  const char *pivot = p + bi;
  mrb_int i;

  for (i = 0; p < pivot; i ++) {
    p += mrb_utf8len(p, e);
  }
  if (p != pivot) return -1;
  return i;
}

static const char *
char_adjust(const char *beg, const char *end, const char *ptr)
{
  if ((ptr > beg || ptr < end) && (*ptr & 0xc0) == 0x80) {
    const int utf8_adjust_max = 3;
    const char *p;

    if (ptr - beg > utf8_adjust_max) {
      beg = ptr - utf8_adjust_max;
    }

    p = ptr;
    while (p > beg) {
      p --;
      if ((*p & 0xc0) != 0x80) {
        int clen = mrb_utf8len(p, end);
        if (clen > ptr - p) return p;
        break;
      }
    }
  }

  return ptr;
}

static const char *
char_backtrack(const char *ptr, const char *end)
{
  if (ptr < end) {
    const int utf8_bytelen_max = 4;
    const char *p;

    if (end - ptr > utf8_bytelen_max) {
      ptr = end - utf8_bytelen_max;
    }

    p = end;
    while (p > ptr) {
      p --;
      if ((*p & 0xc0) != 0x80) {
        int clen = utf8len_codepage[(unsigned char)*p];
        if (clen == end - p) { return p; }
        break;
      }
    }
  }

  return end - 1;
}

static mrb_int
str_index_str_by_char_search(mrb_state *mrb, const char *p, const char *pend, const char *s, const mrb_int slen, mrb_int off)
{
  /* Based on Quick Search algorithm (Boyer-Moore-Horspool algorithm) */

  ptrdiff_t qstable[1 << CHAR_BIT];

  /* Preprocessing */
  {
    mrb_int i;

    for (i = 0; i < 1 << CHAR_BIT; i ++) {
      qstable[i] = slen;
    }
    for (i = 0; i < slen; i ++) {
      qstable[(unsigned char)s[i]] = slen - (i + 1);
    }
  }

  /* Searching */
  while (p < pend && pend - p >= slen) {
    const char *pivot;

    if (memcmp(p, s, slen) == 0) {
      return off;
    }

    pivot = p + qstable[(unsigned char)p[slen - 1]];
    if (pivot >= pend || pivot < p /* overflowed */) { return -1; }

    do {
      p += mrb_utf8len(p, pend);
      off ++;
    } while (p < pivot);
  }

  return -1;
}

static mrb_int
str_index_str_by_char(mrb_state *mrb, mrb_value str, mrb_value sub, mrb_int pos)
{
  const char *p = RSTRING_PTR(str);
  const char *pend = p + RSTRING_LEN(str);
  const char *s = RSTRING_PTR(sub);
  const mrb_int slen = RSTRING_LEN(sub);
  mrb_int off = pos;

  for (; pos > 0; pos --) {
    if (pend - p < 1) { return -1; }
    p += mrb_utf8len(p, pend);
  }

  if (slen < 1) { return off; }

  return str_index_str_by_char_search(mrb, p, pend, s, slen, off);
}

#define BYTES_ALIGN_CHECK(pos) if (pos < 0) return mrb_nil_value();
#else
#define RSTRING_CHAR_LEN(s) RSTRING_LEN(s)
#define chars2bytes(p, off, ci) (ci)
#define bytes2chars(p, end, bi) (bi)
#define char_adjust(beg, end, ptr) (ptr)
#define char_backtrack(ptr, end) ((end) - 1)
#define BYTES_ALIGN_CHECK(pos)
#define str_index_str_by_char(mrb, str, sub, pos) str_index_str(mrb, str, sub, pos)
#endif

#ifndef MRB_QS_SHORT_STRING_LENGTH
#define MRB_QS_SHORT_STRING_LENGTH 2048
#endif

static inline mrb_int
mrb_memsearch_qs(const unsigned char *xs, mrb_int m, const unsigned char *ys, mrb_int n)
{
  if (n + m < MRB_QS_SHORT_STRING_LENGTH) {
    const unsigned char *y = ys;
    const unsigned char *ye = ys+n-m+1;

    for (;;) {
      y = (const unsigned char*)memchr(y, xs[0], (size_t)(ye-y));
      if (y == NULL) return -1;
      if (memcmp(xs, y, m) == 0) {
        return (mrb_int)(y - ys);
      }
      y++;
    }
    return -1;
  }
  else {
    const unsigned char *x = xs, *xe = xs + m;
    const unsigned char *y = ys;
    int i;
    ptrdiff_t qstable[256];

    /* Preprocessing */
    for (i = 0; i < 256; ++i)
      qstable[i] = m + 1;
    for (; x < xe; ++x)
      qstable[*x] = xe - x;
    /* Searching */
    for (; y + m <= ys + n; y += *(qstable + y[m])) {
      if (*xs == *y && memcmp(xs, y, m) == 0)
        return (mrb_int)(y - ys);
    }
    return -1;
  }
}

static mrb_int
mrb_memsearch(const void *x0, mrb_int m, const void *y0, mrb_int n)
{
  const unsigned char *x = (const unsigned char *)x0, *y = (const unsigned char *)y0;

  if (m > n) return -1;
  else if (m == n) {
    return memcmp(x0, y0, m) == 0 ? 0 : -1;
  }
  else if (m < 1) {
    return 0;
  }
  else if (m == 1) {
    const unsigned char *ys = (const unsigned char *)memchr(y, *x, n);

    if (ys)
      return (mrb_int)(ys - y);
    else
      return -1;
  }
  return mrb_memsearch_qs((const unsigned char *)x0, m, (const unsigned char *)y0, n);
}

static void
str_share(mrb_state *mrb, struct RString *orig, struct RString *s)
{
  size_t len = (size_t)orig->as.heap.len;

  mrb_assert(!RSTR_EMBED_P(orig));
  if (RSTR_NOFREE_P(orig)) {
    str_init_nofree(s, orig->as.heap.ptr, len);
  }
  else if (RSTR_SHARED_P(orig)) {
    str_init_shared(mrb, orig, s, orig->as.heap.aux.shared);
  }
  else if (RSTR_FSHARED_P(orig)) {
    str_init_fshared(orig, s, orig->as.heap.aux.fshared);
  }
  else if (mrb_frozen_p(orig) && !RSTR_POOL_P(orig)) {
    str_init_fshared(orig, s, orig);
  }
  else {
    if (orig->as.heap.aux.capa > orig->as.heap.len) {
      orig->as.heap.ptr = (char *)mrb_realloc(mrb, orig->as.heap.ptr, len+1);
      orig->as.heap.aux.capa = (mrb_ssize)len;
    }
    str_init_shared(mrb, orig, s, NULL);
    str_init_shared(mrb, orig, orig, s->as.heap.aux.shared);
  }
}

mrb_value
mrb_str_pool(mrb_state *mrb, const char *p, mrb_int len, mrb_bool nofree)
{
  struct RString *s = (struct RString *)mrb_malloc(mrb, sizeof(struct RString));

  s->tt = MRB_TT_STRING;
  s->c = mrb->string_class;
  s->flags = 0;

  if (RSTR_EMBEDDABLE_P(len)) {
    str_init_embed(s, p, len);
  }
  else if (nofree) {
    str_init_nofree(s, p, len);
  }
  else {
    str_init_normal(mrb, s, p, len);
  }
  RSTR_SET_POOL_FLAG(s);
  MRB_SET_FROZEN_FLAG(s);
  return mrb_obj_value(s);
}

mrb_value
mrb_str_byte_subseq(mrb_state *mrb, mrb_value str, mrb_int beg, mrb_int len)
{
  struct RString *orig, *s;

  orig = mrb_str_ptr(str);
  s = mrb_obj_alloc_string(mrb);
  if (RSTR_EMBEDDABLE_P(len)) {
    str_init_embed(s, RSTR_PTR(orig)+beg, len);
  }
  else {
    str_share(mrb, orig, s);
    s->as.heap.ptr += (mrb_ssize)beg;
    s->as.heap.len = (mrb_ssize)len;
  }
  RSTR_COPY_ASCII_FLAG(s, orig);
  return mrb_obj_value(s);
}

static void
str_range_to_bytes(mrb_value str, mrb_int *pos, mrb_int *len)
{
  *pos = chars2bytes(str, 0, *pos);
  *len = chars2bytes(str, *pos, *len);
}
#ifdef MRB_UTF8_STRING
static inline mrb_value
str_subseq(mrb_state *mrb, mrb_value str, mrb_int beg, mrb_int len)
{
  str_range_to_bytes(str, &beg, &len);
  return mrb_str_byte_subseq(mrb, str, beg, len);
}
#else
#define str_subseq(mrb, str, beg, len) mrb_str_byte_subseq(mrb, str, beg, len)
#endif

mrb_bool
mrb_str_beg_len(mrb_int str_len, mrb_int *begp, mrb_int *lenp)
{
  if (str_len < *begp || *lenp < 0) return FALSE;
  if (*begp < 0) {
    *begp += str_len;
    if (*begp < 0) return FALSE;
  }
  if (*lenp > str_len - *begp)
    *lenp = str_len - *begp;
  if (*lenp <= 0) {
    *lenp = 0;
  }
  return TRUE;
}

static mrb_value
str_substr(mrb_state *mrb, mrb_value str, mrb_int beg, mrb_int len)
{
  return mrb_str_beg_len(RSTRING_CHAR_LEN(str), &beg, &len) ?
    str_subseq(mrb, str, beg, len) : mrb_nil_value();
}

MRB_API mrb_int
mrb_str_index(mrb_state *mrb, mrb_value str, const char *sptr, mrb_int slen, mrb_int offset)
{
  mrb_int pos;
  char *s;
  mrb_int len;

  len = RSTRING_LEN(str);
  if (offset < 0) {
    offset += len;
    if (offset < 0) return -1;
  }
  if (len - offset < slen) return -1;
  s = RSTRING_PTR(str);
  if (offset) {
    s += offset;
  }
  if (slen == 0) return offset;
  /* need proceed one character at a time */
  len = RSTRING_LEN(str) - offset;
  pos = mrb_memsearch(sptr, slen, s, len);
  if (pos < 0) return pos;
  return pos + offset;
}

static mrb_int
str_index_str(mrb_state *mrb, mrb_value str, mrb_value str2, mrb_int offset)
{
  const char *ptr;
  mrb_int len;

  ptr = RSTRING_PTR(str2);
  len = RSTRING_LEN(str2);

  return mrb_str_index(mrb, str, ptr, len, offset);
}

static mrb_value
str_replace(mrb_state *mrb, struct RString *s1, struct RString *s2)
{
  size_t len;

  mrb_check_frozen(mrb, s1);
  if (s1 == s2) return mrb_obj_value(s1);
  RSTR_COPY_ASCII_FLAG(s1, s2);
  if (RSTR_SHARED_P(s1)) {
    str_decref(mrb, s1->as.heap.aux.shared);
  }
  else if (!RSTR_EMBED_P(s1) && !RSTR_NOFREE_P(s1) && !RSTR_FSHARED_P(s1)
           && s1->as.heap.ptr) {
    mrb_free(mrb, s1->as.heap.ptr);
  }

  len = (size_t)RSTR_LEN(s2);
  if (RSTR_EMBEDDABLE_P(len)) {
    str_init_embed(s1, RSTR_PTR(s2), len);
  }
  else {
    str_share(mrb, s2, s1);
  }

  return mrb_obj_value(s1);
}

static mrb_int
str_rindex(mrb_state *mrb, mrb_value str, mrb_value sub, mrb_int pos)
{
  const char *s, *sbeg, *t;
  struct RString *ps = mrb_str_ptr(str);
  mrb_int len = RSTRING_LEN(sub);

  /* substring longer than string */
  if (RSTR_LEN(ps) < len) return -1;
  if (RSTR_LEN(ps) - pos < len) {
    pos = RSTR_LEN(ps) - len;
  }
  sbeg = RSTR_PTR(ps);
  s = RSTR_PTR(ps) + pos;
  t = RSTRING_PTR(sub);
  if (len) {
    s = char_adjust(sbeg, sbeg + RSTR_LEN(ps), s);
    while (sbeg <= s) {
      if (memcmp(s, t, len) == 0) {
        return (mrb_int)(s - RSTR_PTR(ps));
      }
      s = char_backtrack(sbeg, s);
    }
    return -1;
  }
  else {
    return pos;
  }
}

MRB_API mrb_int
mrb_str_strlen(mrb_state *mrb, struct RString *s)
{
  mrb_int i, max = RSTR_LEN(s);
  char *p = RSTR_PTR(s);

  if (!p) return 0;
  for (i=0; i<max; i++) {
    if (p[i] == '\0') {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "string contains null byte");
    }
  }
  return max;
}

#ifdef _WIN32
#include <windows.h>

char*
mrb_utf8_from_locale(const char *str, int len)
{
  wchar_t* wcsp;
  char* mbsp;
  int mbssize, wcssize;

  if (len == 0)
    return strdup("");
  if (len == -1)
    len = (int)strlen(str);
  wcssize = MultiByteToWideChar(GetACP(), 0, str, len,  NULL, 0);
  wcsp = (wchar_t*) malloc((wcssize + 1) * sizeof(wchar_t));
  if (!wcsp)
    return NULL;
  wcssize = MultiByteToWideChar(GetACP(), 0, str, len, wcsp, wcssize + 1);
  wcsp[wcssize] = 0;

  mbssize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR) wcsp, -1, NULL, 0, NULL, NULL);
  mbsp = (char*) malloc((mbssize + 1));
  if (!mbsp) {
    free(wcsp);
    return NULL;
  }
  mbssize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR) wcsp, -1, mbsp, mbssize, NULL, NULL);
  mbsp[mbssize] = 0;
  free(wcsp);
  return mbsp;
}

char*
mrb_locale_from_utf8(const char *utf8, int len)
{
  wchar_t* wcsp;
  char* mbsp;
  int mbssize, wcssize;

  if (len == 0)
    return strdup("");
  if (len == -1)
    len = (int)strlen(utf8);
  wcssize = MultiByteToWideChar(CP_UTF8, 0, utf8, len,  NULL, 0);
  wcsp = (wchar_t*) malloc((wcssize + 1) * sizeof(wchar_t));
  if (!wcsp)
    return NULL;
  wcssize = MultiByteToWideChar(CP_UTF8, 0, utf8, len, wcsp, wcssize + 1);
  wcsp[wcssize] = 0;
  mbssize = WideCharToMultiByte(GetACP(), 0, (LPCWSTR) wcsp, -1, NULL, 0, NULL, NULL);
  mbsp = (char*) malloc((mbssize + 1));
  if (!mbsp) {
    free(wcsp);
    return NULL;
  }
  mbssize = WideCharToMultiByte(GetACP(), 0, (LPCWSTR) wcsp, -1, mbsp, mbssize, NULL, NULL);
  mbsp[mbssize] = 0;
  free(wcsp);
  return mbsp;
}
#endif

MRB_API void
mrb_str_modify_keep_ascii(mrb_state *mrb, struct RString *s)
{
  mrb_check_frozen(mrb, s);
  str_modify_keep_ascii(mrb, s);
}

MRB_API void
mrb_str_modify(mrb_state *mrb, struct RString *s)
{
  mrb_str_modify_keep_ascii(mrb, s);
  RSTR_UNSET_ASCII_FLAG(s);
}

MRB_API mrb_value
mrb_str_resize(mrb_state *mrb, mrb_value str, mrb_int len)
{
  mrb_int slen;
  struct RString *s = mrb_str_ptr(str);

  if (len < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative (or overflowed) string size");
  }
  mrb_str_modify(mrb, s);
  slen = RSTR_LEN(s);
  if (len != slen) {
    if (slen < len || slen - len > 256) {
      resize_capa(mrb, s, len);
    }
    RSTR_SET_LEN(s, len);
    RSTR_PTR(s)[len] = '\0';   /* sentinel */
  }
  return str;
}

MRB_API char*
mrb_str_to_cstr(mrb_state *mrb, mrb_value str0)
{
  struct RString *s;

  check_null_byte(mrb, str0);
  s = str_new(mrb, RSTRING_PTR(str0), RSTRING_LEN(str0));
  return RSTR_PTR(s);
}

MRB_API void
mrb_str_concat(mrb_state *mrb, mrb_value self, mrb_value other)
{
  other = mrb_str_to_str(mrb, other);
  mrb_str_cat_str(mrb, self, other);
}

MRB_API mrb_value
mrb_str_plus(mrb_state *mrb, mrb_value a, mrb_value b)
{
  struct RString *s = mrb_str_ptr(a);
  struct RString *s2 = mrb_str_ptr(b);
  struct RString *t;

  t = str_new(mrb, 0, RSTR_LEN(s) + RSTR_LEN(s2));
  memcpy(RSTR_PTR(t), RSTR_PTR(s), RSTR_LEN(s));
  memcpy(RSTR_PTR(t) + RSTR_LEN(s), RSTR_PTR(s2), RSTR_LEN(s2));

  return mrb_obj_value(t);
}

/* 15.2.10.5.2  */

/*
 *  call-seq:
 *     str + other_str   -> new_str
 *
 *  Concatenation---Returns a new <code>String</code> containing
 *  <i>other_str</i> concatenated to <i>str</i>.
 *
 *     "Hello from " + self.to_s   #=> "Hello from main"
 */
static mrb_value
mrb_str_plus_m(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  mrb_get_args(mrb, "S", &str);
  return mrb_str_plus(mrb, self, str);
}

/* 15.2.10.5.26 */
/* 15.2.10.5.33 */
/*
 *  call-seq:
 *     "abcd".size   => int
 *
 *  Returns the length of string.
 */
static mrb_value
mrb_str_size(mrb_state *mrb, mrb_value self)
{
  mrb_int len = RSTRING_CHAR_LEN(self);
  return mrb_fixnum_value(len);
}

static mrb_value
mrb_str_bytesize(mrb_state *mrb, mrb_value self)
{
  mrb_int len = RSTRING_LEN(self);
  return mrb_fixnum_value(len);
}

/* 15.2.10.5.1  */
/*
 *  call-seq:
 *     str * integer   => new_str
 *
 *  Copy---Returns a new <code>String</code> containing <i>integer</i> copies of
 *  the receiver.
 *
 *     "Ho! " * 3   #=> "Ho! Ho! Ho! "
 */
static mrb_value
mrb_str_times(mrb_state *mrb, mrb_value self)
{
  mrb_int n,len,times;
  struct RString *str2;
  char *p;

  mrb_get_args(mrb, "i", &times);
  if (times < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative argument");
  }
  if (times && MRB_SSIZE_MAX / times < RSTRING_LEN(self)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "argument too big");
  }

  len = RSTRING_LEN(self)*times;
  str2 = str_new(mrb, 0, len);
  str_with_class(str2, self);
  p = RSTR_PTR(str2);
  if (len > 0) {
    n = RSTRING_LEN(self);
    memcpy(p, RSTRING_PTR(self), n);
    while (n <= len/2) {
      memcpy(p + n, p, n);
      n *= 2;
    }
    memcpy(p + n, p, len-n);
  }
  p[RSTR_LEN(str2)] = '\0';
  RSTR_COPY_ASCII_FLAG(str2, mrb_str_ptr(self));

  return mrb_obj_value(str2);
}
/* -------------------------------------------------------------- */

#define lesser(a,b) (((a)>(b))?(b):(a))

/* ---------------------------*/
/*
 *  call-seq:
 *     mrb_value str1 <=> mrb_value str2   => int
 *                     >  1
 *                     =  0
 *                     <  -1
 */
MRB_API int
mrb_str_cmp(mrb_state *mrb, mrb_value str1, mrb_value str2)
{
  mrb_int len;
  mrb_int retval;
  struct RString *s1 = mrb_str_ptr(str1);
  struct RString *s2 = mrb_str_ptr(str2);

  len = lesser(RSTR_LEN(s1), RSTR_LEN(s2));
  retval = memcmp(RSTR_PTR(s1), RSTR_PTR(s2), len);
  if (retval == 0) {
    if (RSTR_LEN(s1) == RSTR_LEN(s2)) return 0;
    if (RSTR_LEN(s1) > RSTR_LEN(s2))  return 1;
    return -1;
  }
  if (retval > 0) return 1;
  return -1;
}

/* 15.2.10.5.3  */

/*
 *  call-seq:
 *     str <=> other_str   => -1, 0, +1
 *
 *  Comparison---Returns -1 if <i>other_str</i> is less than, 0 if
 *  <i>other_str</i> is equal to, and +1 if <i>other_str</i> is greater than
 *  <i>str</i>. If the strings are of different lengths, and the strings are
 *  equal when compared up to the shortest length, then the longer string is
 *  considered greater than the shorter one. If the variable <code>$=</code> is
 *  <code>false</code>, the comparison is based on comparing the binary values
 *  of each character in the string. In older versions of Ruby, setting
 *  <code>$=</code> allowed case-insensitive comparisons; this is now deprecated
 *  in favor of using <code>String#casecmp</code>.
 *
 *  <code><=></code> is the basis for the methods <code><</code>,
 *  <code><=</code>, <code>></code>, <code>>=</code>, and <code>between?</code>,
 *  included from module <code>Comparable</code>.  The method
 *  <code>String#==</code> does not use <code>Comparable#==</code>.
 *
 *     "abcdef" <=> "abcde"     #=> 1
 *     "abcdef" <=> "abcdef"    #=> 0
 *     "abcdef" <=> "abcdefg"   #=> -1
 *     "abcdef" <=> "ABCDEF"    #=> 1
 */
static mrb_value
mrb_str_cmp_m(mrb_state *mrb, mrb_value str1)
{
  mrb_value str2 = mrb_get_arg1(mrb);
  mrb_int result;

  if (!mrb_string_p(str2)) {
    return mrb_nil_value();
  }
  else {
    result = mrb_str_cmp(mrb, str1, str2);
  }
  return mrb_fixnum_value(result);
}

static mrb_bool
str_eql(mrb_state *mrb, const mrb_value str1, const mrb_value str2)
{
  const mrb_int len = RSTRING_LEN(str1);

  if (len != RSTRING_LEN(str2)) return FALSE;
  if (memcmp(RSTRING_PTR(str1), RSTRING_PTR(str2), (size_t)len) == 0)
    return TRUE;
  return FALSE;
}

MRB_API mrb_bool
mrb_str_equal(mrb_state *mrb, mrb_value str1, mrb_value str2)
{
  if (!mrb_string_p(str2)) return FALSE;
  return str_eql(mrb, str1, str2);
}

/* 15.2.10.5.4  */
/*
 *  call-seq:
 *     str == obj   => true or false
 *
 *  Equality---
 *  If <i>obj</i> is not a <code>String</code>, returns <code>false</code>.
 *  Otherwise, returns <code>false</code> or <code>true</code>
 *
 *   caution:if <i>str</i> <code><=></code> <i>obj</i> returns zero.
 */
static mrb_value
mrb_str_equal_m(mrb_state *mrb, mrb_value str1)
{
  mrb_value str2 = mrb_get_arg1(mrb);

  return mrb_bool_value(mrb_str_equal(mrb, str1, str2));
}
/* ---------------------------------- */

MRB_API mrb_value
mrb_str_to_str(mrb_state *mrb, mrb_value str)
{
  switch (mrb_type(str)) {
  case MRB_TT_STRING:
    return str;
  case MRB_TT_SYMBOL:
    return mrb_sym_str(mrb, mrb_symbol(str));
  case MRB_TT_FIXNUM:
    return mrb_fixnum_to_str(mrb, str, 10);
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
    return mrb_mod_to_s(mrb, str);
  default:
    return mrb_convert_type(mrb, str, MRB_TT_STRING, "String", "to_s");
  }
}

/* obslete: use RSTRING_PTR() */
MRB_API const char*
mrb_string_value_ptr(mrb_state *mrb, mrb_value str)
{
  str = mrb_str_to_str(mrb, str);
  return RSTRING_PTR(str);
}

/* obslete: use RSTRING_LEN() */
MRB_API mrb_int
mrb_string_value_len(mrb_state *mrb, mrb_value ptr)
{
  mrb_to_str(mrb, ptr);
  return RSTRING_LEN(ptr);
}

MRB_API mrb_value
mrb_str_dup(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  struct RString *dup = str_new(mrb, 0, 0);

  str_with_class(dup, str);
  return str_replace(mrb, dup, s);
}

enum str_convert_range {
  /* `beg` and `len` are byte unit in `0 ... str.bytesize` */
  STR_BYTE_RANGE_CORRECTED = 1,

  /* `beg` and `len` are char unit in any range */
  STR_CHAR_RANGE = 2,

  /* `beg` and `len` are char unit in `0 ... str.size` */
  STR_CHAR_RANGE_CORRECTED = 3,

  /* `beg` is out of range */
  STR_OUT_OF_RANGE = -1
};

static enum str_convert_range
str_convert_range(mrb_state *mrb, mrb_value str, mrb_value indx, mrb_value alen, mrb_int *beg, mrb_int *len)
{
  if (!mrb_undef_p(alen)) {
    *beg = mrb_int(mrb, indx);
    *len = mrb_int(mrb, alen);
    return STR_CHAR_RANGE;
  }
  else {
    switch (mrb_type(indx)) {
      case MRB_TT_FIXNUM:
        *beg = mrb_fixnum(indx);
        *len = 1;
        return STR_CHAR_RANGE;

      case MRB_TT_STRING:
        *beg = str_index_str(mrb, str, indx, 0);
        if (*beg < 0) { break; }
        *len = RSTRING_LEN(indx);
        return STR_BYTE_RANGE_CORRECTED;

      case MRB_TT_RANGE:
        goto range_arg;

      default:
        indx = mrb_to_int(mrb, indx);
        if (mrb_fixnum_p(indx)) {
          *beg = mrb_fixnum(indx);
          *len = 1;
          return STR_CHAR_RANGE;
        }
range_arg:
        *len = RSTRING_CHAR_LEN(str);
        switch (mrb_range_beg_len(mrb, indx, beg, len, *len, TRUE)) {
          case MRB_RANGE_OK:
            return STR_CHAR_RANGE_CORRECTED;
          case MRB_RANGE_OUT:
            return STR_OUT_OF_RANGE;
          default:
            break;
        }

        mrb_raise(mrb, E_TYPE_ERROR, "can't convert to Fixnum");
    }
  }
  return STR_OUT_OF_RANGE;
}

static mrb_value
mrb_str_aref(mrb_state *mrb, mrb_value str, mrb_value indx, mrb_value alen)
{
  mrb_int beg, len;

  switch (str_convert_range(mrb, str, indx, alen, &beg, &len)) {
    case STR_CHAR_RANGE_CORRECTED:
      return str_subseq(mrb, str, beg, len);
    case STR_CHAR_RANGE:
      str = str_substr(mrb, str, beg, len);
      if (mrb_undef_p(alen) && !mrb_nil_p(str) && RSTRING_LEN(str) == 0) return mrb_nil_value();
      return str;
    case STR_BYTE_RANGE_CORRECTED:
      if (mrb_string_p(indx)) {
        return mrb_str_dup(mrb, indx);
      }
      else {
        return mrb_str_byte_subseq(mrb, str, beg, len);
      }
    case STR_OUT_OF_RANGE:
    default:
      return mrb_nil_value();
  }
}

/* 15.2.10.5.6  */
/* 15.2.10.5.34 */
/*
 *  call-seq:
 *     str[fixnum]                 => fixnum or nil
 *     str[fixnum, fixnum]         => new_str or nil
 *     str[range]                  => new_str or nil
 *     str[other_str]              => new_str or nil
 *     str.slice(fixnum)           => fixnum or nil
 *     str.slice(fixnum, fixnum)   => new_str or nil
 *     str.slice(range)            => new_str or nil
 *     str.slice(other_str)        => new_str or nil
 *
 *  Element Reference---If passed a single <code>Fixnum</code>, returns the code
 *  of the character at that position. If passed two <code>Fixnum</code>
 *  objects, returns a substring starting at the offset given by the first, and
 *  a length given by the second. If given a range, a substring containing
 *  characters at offsets given by the range is returned. In all three cases, if
 *  an offset is negative, it is counted from the end of <i>str</i>. Returns
 *  <code>nil</code> if the initial offset falls outside the string, the length
 *  is negative, or the beginning of the range is greater than the end.
 *
 *  If a <code>String</code> is given, that string is returned if it occurs in
 *  <i>str</i>. In both cases, <code>nil</code> is returned if there is no
 *  match.
 *
 *     a = "hello there"
 *     a[1]                   #=> 101(1.8.7) "e"(1.9.2)
 *     a[1.1]                 #=>            "e"(1.9.2)
 *     a[1,3]                 #=> "ell"
 *     a[1..3]                #=> "ell"
 *     a[-3,2]                #=> "er"
 *     a[-4..-2]              #=> "her"
 *     a[12..-1]              #=> nil
 *     a[-2..-4]              #=> ""
 *     a["lo"]                #=> "lo"
 *     a["bye"]               #=> nil
 */
static mrb_value
mrb_str_aref_m(mrb_state *mrb, mrb_value str)
{
  mrb_value a1, a2;

  if (mrb_get_args(mrb, "o|o", &a1, &a2) == 1) {
    a2 = mrb_undef_value();
  }

  return mrb_str_aref(mrb, str, a1, a2);
}

static mrb_noreturn void
str_out_of_index(mrb_state *mrb, mrb_value index)
{
  mrb_raisef(mrb, E_INDEX_ERROR, "index %v out of string", index);
}

static mrb_value
str_replace_partial(mrb_state *mrb, mrb_value src, mrb_int pos, mrb_int end, mrb_value rep)
{
  const mrb_int shrink_threshold = 256;
  struct RString *str = mrb_str_ptr(src);
  mrb_int len = RSTR_LEN(str);
  mrb_int replen, newlen;
  char *strp;

  if (end > len) { end = len; }

  if (pos < 0 || pos > len) {
    str_out_of_index(mrb, mrb_fixnum_value(pos));
  }

  replen = (mrb_nil_p(rep) ? 0 : RSTRING_LEN(rep));
  newlen = replen + len - (end - pos);

  if (newlen >= MRB_SSIZE_MAX || newlen < replen /* overflowed */) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "string size too big");
  }

  mrb_str_modify(mrb, str);

  if (len < newlen) {
    resize_capa(mrb, str, newlen);
  }

  strp = RSTR_PTR(str);

  memmove(strp + newlen - (len - end), strp + end, len - end);
  if (!mrb_nil_p(rep)) {
    memmove(strp + pos, RSTRING_PTR(rep), replen);
  }
  RSTR_SET_LEN(str, newlen);
  strp[newlen] = '\0';

  if (len - newlen >= shrink_threshold) {
    resize_capa(mrb, str, newlen);
  }

  return src;
}

#define IS_EVSTR(p,e) ((p) < (e) && (*(p) == '$' || *(p) == '@' || *(p) == '{'))

static mrb_value
str_escape(mrb_state *mrb, mrb_value str, mrb_bool inspect)
{
  const char *p, *pend;
  char buf[4];  /* `\x??` or UTF-8 character */
  mrb_value result = mrb_str_new_lit(mrb, "\"");
#ifdef MRB_UTF8_STRING
  uint32_t ascii_flag = MRB_STR_ASCII;
#endif

  p = RSTRING_PTR(str); pend = RSTRING_END(str);
  for (;p < pend; p++) {
    unsigned char c, cc;
#ifdef MRB_UTF8_STRING
    if (inspect) {
      mrb_int clen = mrb_utf8len(p, pend);
      if (clen > 1) {
        mrb_int i;

        for (i=0; i<clen; i++) {
          buf[i] = p[i];
        }
        mrb_str_cat(mrb, result, buf, clen);
        p += clen-1;
        ascii_flag = 0;
        continue;
      }
    }
#endif
    c = *p;
    if (c == '"'|| c == '\\' || (c == '#' && IS_EVSTR(p+1, pend))) {
      buf[0] = '\\'; buf[1] = c;
      mrb_str_cat(mrb, result, buf, 2);
      continue;
    }
    if (ISPRINT(c)) {
      buf[0] = c;
      mrb_str_cat(mrb, result, buf, 1);
      continue;
    }
    switch (c) {
      case '\n': cc = 'n'; break;
      case '\r': cc = 'r'; break;
      case '\t': cc = 't'; break;
      case '\f': cc = 'f'; break;
      case '\013': cc = 'v'; break;
      case '\010': cc = 'b'; break;
      case '\007': cc = 'a'; break;
      case 033: cc = 'e'; break;
      default: cc = 0; break;
    }
    if (cc) {
      buf[0] = '\\';
      buf[1] = (char)cc;
      mrb_str_cat(mrb, result, buf, 2);
      continue;
    }
    else {
      buf[0] = '\\';
      buf[1] = 'x';
      buf[3] = mrb_digitmap[c % 16]; c /= 16;
      buf[2] = mrb_digitmap[c % 16];
      mrb_str_cat(mrb, result, buf, 4);
      continue;
    }
  }
  mrb_str_cat_lit(mrb, result, "\"");
#ifdef MRB_UTF8_STRING
  if (inspect) {
    mrb_str_ptr(str)->flags |= ascii_flag;
    mrb_str_ptr(result)->flags |= ascii_flag;
  }
  else {
    RSTR_SET_ASCII_FLAG(mrb_str_ptr(result));
  }
#endif

  return result;
}

static void
mrb_str_aset(mrb_state *mrb, mrb_value str, mrb_value indx, mrb_value alen, mrb_value replace)
{
  mrb_int beg, len, charlen;

  mrb_to_str(mrb, replace);

  switch (str_convert_range(mrb, str, indx, alen, &beg, &len)) {
    case STR_OUT_OF_RANGE:
    default:
      mrb_raise(mrb, E_INDEX_ERROR, "string not matched");
    case STR_CHAR_RANGE:
      if (len < 0) {
        mrb_raisef(mrb, E_INDEX_ERROR, "negative length %v", alen);
      }
      charlen = RSTRING_CHAR_LEN(str);
      if (beg < 0) { beg += charlen; }
      if (beg < 0 || beg > charlen) { str_out_of_index(mrb, indx); }
      /* fall through */
    case STR_CHAR_RANGE_CORRECTED:
      str_range_to_bytes(str, &beg, &len);
      /* fall through */
    case STR_BYTE_RANGE_CORRECTED:
      str_replace_partial(mrb, str, beg, beg + len, replace);
  }
}

/*
 * call-seq:
 *    str[fixnum] = replace
 *    str[fixnum, fixnum] = replace
 *    str[range] = replace
 *    str[other_str] = replace
 *
 * Modify +self+ by replacing the content of +self+.
 * The portion of the string affected is determined using the same criteria as +String#[]+.
 */
static mrb_value
mrb_str_aset_m(mrb_state *mrb, mrb_value str)
{
  mrb_value indx, alen, replace;

  switch (mrb_get_args(mrb, "oo|S!", &indx, &alen, &replace)) {
    case 2:
      replace = alen;
      alen = mrb_undef_value();
      break;
    case 3:
      break;
  }
  mrb_str_aset(mrb, str, indx, alen, replace);
  return str;
}

/* 15.2.10.5.8  */
/*
 *  call-seq:
 *     str.capitalize!   => str or nil
 *
 *  Modifies <i>str</i> by converting the first character to uppercase and the
 *  remainder to lowercase. Returns <code>nil</code> if no changes are made.
 *
 *     a = "hello"
 *     a.capitalize!   #=> "Hello"
 *     a               #=> "Hello"
 *     a.capitalize!   #=> nil
 */
static mrb_value
mrb_str_capitalize_bang(mrb_state *mrb, mrb_value str)
{
  char *p, *pend;
  mrb_bool modify = FALSE;
  struct RString *s = mrb_str_ptr(str);

  mrb_str_modify_keep_ascii(mrb, s);
  if (RSTR_LEN(s) == 0 || !RSTR_PTR(s)) return mrb_nil_value();
  p = RSTR_PTR(s); pend = RSTR_PTR(s) + RSTR_LEN(s);
  if (ISLOWER(*p)) {
    *p = TOUPPER(*p);
    modify = TRUE;
  }
  while (++p < pend) {
    if (ISUPPER(*p)) {
      *p = TOLOWER(*p);
      modify = TRUE;
    }
  }
  if (modify) return str;
  return mrb_nil_value();
}

/* 15.2.10.5.7  */
/*
 *  call-seq:
 *     str.capitalize   => new_str
 *
 *  Returns a copy of <i>str</i> with the first character converted to uppercase
 *  and the remainder to lowercase.
 *
 *     "hello".capitalize    #=> "Hello"
 *     "HELLO".capitalize    #=> "Hello"
 *     "123ABC".capitalize   #=> "123abc"
 */
static mrb_value
mrb_str_capitalize(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  str = mrb_str_dup(mrb, self);
  mrb_str_capitalize_bang(mrb, str);
  return str;
}

/* 15.2.10.5.10  */
/*
 *  call-seq:
 *     str.chomp!(separator="\n")   => str or nil
 *
 *  Modifies <i>str</i> in place as described for <code>String#chomp</code>,
 *  returning <i>str</i>, or <code>nil</code> if no modifications were made.
 */
static mrb_value
mrb_str_chomp_bang(mrb_state *mrb, mrb_value str)
{
  mrb_value rs;
  mrb_int newline;
  char *p, *pp;
  mrb_int rslen;
  mrb_int len;
  mrb_int argc;
  struct RString *s = mrb_str_ptr(str);

  argc = mrb_get_args(mrb, "|S", &rs);
  mrb_str_modify_keep_ascii(mrb, s);
  len = RSTR_LEN(s);
  if (argc == 0) {
    if (len == 0) return mrb_nil_value();
  smart_chomp:
    if (RSTR_PTR(s)[len-1] == '\n') {
      RSTR_SET_LEN(s, RSTR_LEN(s) - 1);
      if (RSTR_LEN(s) > 0 &&
          RSTR_PTR(s)[RSTR_LEN(s)-1] == '\r') {
        RSTR_SET_LEN(s, RSTR_LEN(s) - 1);
      }
    }
    else if (RSTR_PTR(s)[len-1] == '\r') {
      RSTR_SET_LEN(s, RSTR_LEN(s) - 1);
    }
    else {
      return mrb_nil_value();
    }
    RSTR_PTR(s)[RSTR_LEN(s)] = '\0';
    return str;
  }

  if (len == 0 || mrb_nil_p(rs)) return mrb_nil_value();
  p = RSTR_PTR(s);
  rslen = RSTRING_LEN(rs);
  if (rslen == 0) {
    while (len>0 && p[len-1] == '\n') {
      len--;
      if (len>0 && p[len-1] == '\r')
        len--;
    }
    if (len < RSTR_LEN(s)) {
      RSTR_SET_LEN(s, len);
      p[len] = '\0';
      return str;
    }
    return mrb_nil_value();
  }
  if (rslen > len) return mrb_nil_value();
  newline = RSTRING_PTR(rs)[rslen-1];
  if (rslen == 1 && newline == '\n')
    newline = RSTRING_PTR(rs)[rslen-1];
  if (rslen == 1 && newline == '\n')
    goto smart_chomp;

  pp = p + len - rslen;
  if (p[len-1] == newline &&
     (rslen <= 1 ||
     memcmp(RSTRING_PTR(rs), pp, rslen) == 0)) {
    RSTR_SET_LEN(s, len - rslen);
    p[RSTR_LEN(s)] = '\0';
    return str;
  }
  return mrb_nil_value();
}

/* 15.2.10.5.9  */
/*
 *  call-seq:
 *     str.chomp(separator="\n")   => new_str
 *
 *  Returns a new <code>String</code> with the given record separator removed
 *  from the end of <i>str</i> (if present). <code>chomp</code> also removes
 *  carriage return characters (that is it will remove <code>\n</code>,
 *  <code>\r</code>, and <code>\r\n</code>).
 *
 *     "hello".chomp            #=> "hello"
 *     "hello\n".chomp          #=> "hello"
 *     "hello\r\n".chomp        #=> "hello"
 *     "hello\n\r".chomp        #=> "hello\n"
 *     "hello\r".chomp          #=> "hello"
 *     "hello \n there".chomp   #=> "hello \n there"
 *     "hello".chomp("llo")     #=> "he"
 */
static mrb_value
mrb_str_chomp(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  str = mrb_str_dup(mrb, self);
  mrb_str_chomp_bang(mrb, str);
  return str;
}

/* 15.2.10.5.12 */
/*
 *  call-seq:
 *     str.chop!   => str or nil
 *
 *  Processes <i>str</i> as for <code>String#chop</code>, returning <i>str</i>,
 *  or <code>nil</code> if <i>str</i> is the empty string.  See also
 *  <code>String#chomp!</code>.
 */
static mrb_value
mrb_str_chop_bang(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);

  mrb_str_modify_keep_ascii(mrb, s);
  if (RSTR_LEN(s) > 0) {
    mrb_int len;
#ifdef MRB_UTF8_STRING
    const char* t = RSTR_PTR(s), *p = t;
    const char* e = p + RSTR_LEN(s);
    while (p<e) {
      mrb_int clen = mrb_utf8len(p, e);
      if (p + clen>=e) break;
      p += clen;
    }
    len = p - t;
#else
    len = RSTR_LEN(s) - 1;
#endif
    if (RSTR_PTR(s)[len] == '\n') {
      if (len > 0 &&
          RSTR_PTR(s)[len-1] == '\r') {
        len--;
      }
    }
    RSTR_SET_LEN(s, len);
    RSTR_PTR(s)[len] = '\0';
    return str;
  }
  return mrb_nil_value();
}

/* 15.2.10.5.11 */
/*
 *  call-seq:
 *     str.chop   => new_str
 *
 *  Returns a new <code>String</code> with the last character removed.  If the
 *  string ends with <code>\r\n</code>, both characters are removed. Applying
 *  <code>chop</code> to an empty string returns an empty
 *  string. <code>String#chomp</code> is often a safer alternative, as it leaves
 *  the string unchanged if it doesn't end in a record separator.
 *
 *     "string\r\n".chop   #=> "string"
 *     "string\n\r".chop   #=> "string\n"
 *     "string\n".chop     #=> "string"
 *     "string".chop       #=> "strin"
 *     "x".chop            #=> ""
 */
static mrb_value
mrb_str_chop(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  str = mrb_str_dup(mrb, self);
  mrb_str_chop_bang(mrb, str);
  return str;
}

/* 15.2.10.5.14 */
/*
 *  call-seq:
 *     str.downcase!   => str or nil
 *
 *  Downcases the contents of <i>str</i>, returning <code>nil</code> if no
 *  changes were made.
 */
static mrb_value
mrb_str_downcase_bang(mrb_state *mrb, mrb_value str)
{
  char *p, *pend;
  mrb_bool modify = FALSE;
  struct RString *s = mrb_str_ptr(str);

  mrb_str_modify_keep_ascii(mrb, s);
  p = RSTR_PTR(s);
  pend = RSTR_PTR(s) + RSTR_LEN(s);
  while (p < pend) {
    if (ISUPPER(*p)) {
      *p = TOLOWER(*p);
      modify = TRUE;
    }
    p++;
  }

  if (modify) return str;
  return mrb_nil_value();
}

/* 15.2.10.5.13 */
/*
 *  call-seq:
 *     str.downcase   => new_str
 *
 *  Returns a copy of <i>str</i> with all uppercase letters replaced with their
 *  lowercase counterparts. The operation is locale insensitive---only
 *  characters 'A' to 'Z' are affected.
 *
 *     "hEllO".downcase   #=> "hello"
 */
static mrb_value
mrb_str_downcase(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  str = mrb_str_dup(mrb, self);
  mrb_str_downcase_bang(mrb, str);
  return str;
}

/* 15.2.10.5.16 */
/*
 *  call-seq:
 *     str.empty?   => true or false
 *
 *  Returns <code>true</code> if <i>str</i> has a length of zero.
 *
 *     "hello".empty?   #=> false
 *     "".empty?        #=> true
 */
static mrb_value
mrb_str_empty_p(mrb_state *mrb, mrb_value self)
{
  struct RString *s = mrb_str_ptr(self);

  return mrb_bool_value(RSTR_LEN(s) == 0);
}

/* 15.2.10.5.17 */
/*
 * call-seq:
 *   str.eql?(other)   => true or false
 *
 * Two strings are equal if the have the same length and content.
 */
static mrb_value
mrb_str_eql(mrb_state *mrb, mrb_value self)
{
  mrb_value str2 = mrb_get_arg1(mrb);
  mrb_bool eql_p;

  eql_p = (mrb_string_p(str2)) && str_eql(mrb, self, str2);

  return mrb_bool_value(eql_p);
}

MRB_API mrb_value
mrb_str_substr(mrb_state *mrb, mrb_value str, mrb_int beg, mrb_int len)
{
  return str_substr(mrb, str, beg, len);
}

uint32_t
mrb_str_hash(mrb_state *mrb, mrb_value str)
{
  /* 1-8-7 */
  struct RString *s = mrb_str_ptr(str);
  mrb_int len = RSTR_LEN(s);
  char *p = RSTR_PTR(s);
  uint64_t key = 0;

  while (len--) {
    key = key*65599 + *p;
    p++;
  }
  return (uint32_t)(key + (key>>5));
}

/* 15.2.10.5.20 */
/*
 * call-seq:
 *    str.hash   => fixnum
 *
 * Return a hash based on the string's length and content.
 */
static mrb_value
mrb_str_hash_m(mrb_state *mrb, mrb_value self)
{
  mrb_int key = mrb_str_hash(mrb, self);
  return mrb_fixnum_value(key);
}

/* 15.2.10.5.21 */
/*
 *  call-seq:
 *     str.include? other_str   => true or false
 *     str.include? fixnum      => true or false
 *
 *  Returns <code>true</code> if <i>str</i> contains the given string or
 *  character.
 *
 *     "hello".include? "lo"   #=> true
 *     "hello".include? "ol"   #=> false
 *     "hello".include? ?h     #=> true
 */
static mrb_value
mrb_str_include(mrb_state *mrb, mrb_value self)
{
  mrb_value str2;

  mrb_get_args(mrb, "S", &str2);
  if (str_index_str(mrb, self, str2, 0) < 0)
    return mrb_bool_value(FALSE);
  return mrb_bool_value(TRUE);
}

/* 15.2.10.5.22 */
/*
 *  call-seq:
 *     str.index(substring [, offset])   => fixnum or nil
 *
 *  Returns the index of the first occurrence of the given
 *  <i>substring</i>. Returns <code>nil</code> if not found.
 *  If the second parameter is present, it
 *  specifies the position in the string to begin the search.
 *
 *     "hello".index('l')             #=> 2
 *     "hello".index('lo')            #=> 3
 *     "hello".index('a')             #=> nil
 *     "hello".index('l', -2)         #=> 3
 */
static mrb_value
mrb_str_index_m(mrb_state *mrb, mrb_value str)
{
  mrb_value sub;
  mrb_int pos;

  if (mrb_get_args(mrb, "S|i", &sub, &pos) == 1) {
    pos = 0;
  }
  else if (pos < 0) {
    mrb_int clen = RSTRING_CHAR_LEN(str);
    pos += clen;
    if (pos < 0) {
      return mrb_nil_value();
    }
  }
  pos = str_index_str_by_char(mrb, str, sub, pos);

  if (pos == -1) return mrb_nil_value();
  BYTES_ALIGN_CHECK(pos);
  return mrb_fixnum_value(pos);
}

/* 15.2.10.5.24 */
/* 15.2.10.5.28 */
/*
 *  call-seq:
 *     str.replace(other_str)   => str
 *
 *     s = "hello"         #=> "hello"
 *     s.replace "world"   #=> "world"
 */
static mrb_value
mrb_str_replace(mrb_state *mrb, mrb_value str)
{
  mrb_value str2;

  mrb_get_args(mrb, "S", &str2);
  return str_replace(mrb, mrb_str_ptr(str), mrb_str_ptr(str2));
}

/* 15.2.10.5.23 */
/*
 *  call-seq:
 *     String.new(str="")   => new_str
 *
 *  Returns a new string object containing a copy of <i>str</i>.
 */
static mrb_value
mrb_str_init(mrb_state *mrb, mrb_value self)
{
  mrb_value str2;

  if (mrb_get_args(mrb, "|S", &str2) == 0) {
    struct RString *s = str_new(mrb, 0, 0);
    str2 = mrb_obj_value(s);
  }
  str_replace(mrb, mrb_str_ptr(self), mrb_str_ptr(str2));
  return self;
}

/* 15.2.10.5.25 */
/* 15.2.10.5.41 */
/*
 *  call-seq:
 *     str.intern   => symbol
 *     str.to_sym   => symbol
 *
 *  Returns the <code>Symbol</code> corresponding to <i>str</i>, creating the
 *  symbol if it did not previously exist. See <code>Symbol#id2name</code>.
 *
 *     "Koala".intern         #=> :Koala
 *     s = 'cat'.to_sym       #=> :cat
 *     s == :cat              #=> true
 *     s = '@cat'.to_sym      #=> :@cat
 *     s == :@cat             #=> true
 *
 *  This can also be used to create symbols that cannot be represented using the
 *  <code>:xxx</code> notation.
 *
 *     'cat and dog'.to_sym   #=> :"cat and dog"
 */
MRB_API mrb_value
mrb_str_intern(mrb_state *mrb, mrb_value self)
{
  return mrb_symbol_value(mrb_intern_str(mrb, self));
}
/* ---------------------------------- */
MRB_API mrb_value
mrb_obj_as_string(mrb_state *mrb, mrb_value obj)
{
  if (mrb_string_p(obj)) {
    return obj;
  }
  return mrb_str_to_str(mrb, obj);
}

MRB_API mrb_value
mrb_ptr_to_str(mrb_state *mrb, void *p)
{
  struct RString *p_str;
  char *p1;
  char *p2;
  uintptr_t n = (uintptr_t)p;

  p_str = str_new(mrb, NULL, 2 + sizeof(uintptr_t) * CHAR_BIT / 4);
  p1 = RSTR_PTR(p_str);
  *p1++ = '0';
  *p1++ = 'x';
  p2 = p1;

  do {
    *p2++ = mrb_digitmap[n % 16];
    n /= 16;
  } while (n > 0);
  *p2 = '\0';
  RSTR_SET_LEN(p_str, (mrb_int)(p2 - RSTR_PTR(p_str)));

  while (p1 < p2) {
    const char  c = *p1;
    *p1++ = *--p2;
    *p2 = c;
  }

  return mrb_obj_value(p_str);
}

static inline void
str_reverse(char *p, char *e)
{
  char c;

  while (p < e) {
    c = *p;
    *p++ = *e;
    *e-- = c;
  }
}

/* 15.2.10.5.30 */
/*
 *  call-seq:
 *     str.reverse!   => str
 *
 *  Reverses <i>str</i> in place.
 */
static mrb_value
mrb_str_reverse_bang(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  char *p, *e;

#ifdef MRB_UTF8_STRING
  mrb_int utf8_len = RSTRING_CHAR_LEN(str);
  mrb_int len = RSTR_LEN(s);

  if (utf8_len < 2) return str;
  if (utf8_len < len) {
    mrb_str_modify(mrb, s);
    p = RSTR_PTR(s);
    e = p + RSTR_LEN(s);
    while (p<e) {
      mrb_int clen = mrb_utf8len(p, e);
      str_reverse(p, p + clen - 1);
      p += clen;
    }
    goto bytes;
  }
#endif

  if (RSTR_LEN(s) > 1) {
    mrb_str_modify(mrb, s);
    goto bytes;
  }
  return str;

 bytes:
  p = RSTR_PTR(s);
  e = p + RSTR_LEN(s) - 1;
  str_reverse(p, e);
  return str;
}

/* ---------------------------------- */
/* 15.2.10.5.29 */
/*
 *  call-seq:
 *     str.reverse   => new_str
 *
 *  Returns a new string with the characters from <i>str</i> in reverse order.
 *
 *     "stressed".reverse   #=> "desserts"
 */
static mrb_value
mrb_str_reverse(mrb_state *mrb, mrb_value str)
{
  mrb_value str2 = mrb_str_dup(mrb, str);
  mrb_str_reverse_bang(mrb, str2);
  return str2;
}

/* 15.2.10.5.31 */
/*
 *  call-seq:
 *     str.rindex(substring [, offset])   => fixnum or nil
 *
 *  Returns the index of the last occurrence of the given <i>substring</i>.
 *  Returns <code>nil</code> if not found. If the second parameter is
 *  present, it specifies the position in the string to end the
 *  search---characters beyond this point will not be considered.
 *
 *     "hello".rindex('e')             #=> 1
 *     "hello".rindex('l')             #=> 3
 *     "hello".rindex('a')             #=> nil
 *     "hello".rindex('l', 2)          #=> 2
 */
static mrb_value
mrb_str_rindex(mrb_state *mrb, mrb_value str)
{
  mrb_value sub;
  mrb_int pos, len = RSTRING_CHAR_LEN(str);

  if (mrb_get_args(mrb, "S|i", &sub, &pos) == 1) {
    pos = len;
  }
  else {
    if (pos < 0) {
      pos += len;
      if (pos < 0) {
        return mrb_nil_value();
      }
    }
    if (pos > len) pos = len;
  }
  pos = chars2bytes(str, 0, pos);
  pos = str_rindex(mrb, str, sub, pos);
  if (pos >= 0) {
    pos = bytes2chars(RSTRING_PTR(str), RSTRING_LEN(str), pos);
    BYTES_ALIGN_CHECK(pos);
    return mrb_fixnum_value(pos);
  }
  return mrb_nil_value();
}

/* 15.2.10.5.35 */

/*
 *  call-seq:
 *     str.split(separator=nil, [limit])   => anArray
 *
 *  Divides <i>str</i> into substrings based on a delimiter, returning an array
 *  of these substrings.
 *
 *  If <i>separator</i> is a <code>String</code>, then its contents are used as
 *  the delimiter when splitting <i>str</i>. If <i>separator</i> is a single
 *  space, <i>str</i> is split on whitespace, with leading whitespace and runs
 *  of contiguous whitespace characters ignored.
 *
 *  If <i>separator</i> is omitted or <code>nil</code> (which is the default),
 *  <i>str</i> is split on whitespace as if ' ' were specified.
 *
 *  If the <i>limit</i> parameter is omitted, trailing null fields are
 *  suppressed. If <i>limit</i> is a positive number, at most that number of
 *  fields will be returned (if <i>limit</i> is <code>1</code>, the entire
 *  string is returned as the only entry in an array). If negative, there is no
 *  limit to the number of fields returned, and trailing null fields are not
 *  suppressed.
 *
 *     " now's  the time".split        #=> ["now's", "the", "time"]
 *     " now's  the time".split(' ')   #=> ["now's", "the", "time"]
 *
 *     "mellow yellow".split("ello")   #=> ["m", "w y", "w"]
 *     "1,2,,3,4,,".split(',')         #=> ["1", "2", "", "3", "4"]
 *     "1,2,,3,4,,".split(',', 4)      #=> ["1", "2", "", "3,4,,"]
 *     "1,2,,3,4,,".split(',', -4)     #=> ["1", "2", "", "3", "4", "", ""]
 */

static mrb_value
mrb_str_split_m(mrb_state *mrb, mrb_value str)
{
  mrb_int argc;
  mrb_value spat = mrb_nil_value();
  enum {awk, string} split_type = string;
  mrb_int i = 0;
  mrb_int beg;
  mrb_int end;
  mrb_int lim = 0;
  mrb_bool lim_p;
  mrb_value result, tmp;

  argc = mrb_get_args(mrb, "|oi", &spat, &lim);
  lim_p = (lim > 0 && argc == 2);
  if (argc == 2) {
    if (lim == 1) {
      if (RSTRING_LEN(str) == 0)
        return mrb_ary_new_capa(mrb, 0);
      return mrb_ary_new_from_values(mrb, 1, &str);
    }
    i = 1;
  }

  if (argc == 0 || mrb_nil_p(spat)) {
    split_type = awk;
  }
  else if (!mrb_string_p(spat)) {
    mrb_raise(mrb, E_TYPE_ERROR, "expected String");
  }
  else if (RSTRING_LEN(spat) == 1 && RSTRING_PTR(spat)[0] == ' ') {
    split_type = awk;
  }

  result = mrb_ary_new(mrb);
  beg = 0;
  if (split_type == awk) {
    mrb_bool skip = TRUE;
    mrb_int idx = 0;
    mrb_int str_len = RSTRING_LEN(str);
    unsigned int c;
    int ai = mrb_gc_arena_save(mrb);

    idx = end = beg;
    while (idx < str_len) {
      c = (unsigned char)RSTRING_PTR(str)[idx++];
      if (skip) {
        if (ISSPACE(c)) {
          beg = idx;
        }
        else {
          end = idx;
          skip = FALSE;
          if (lim_p && lim <= i) break;
        }
      }
      else if (ISSPACE(c)) {
        mrb_ary_push(mrb, result, mrb_str_byte_subseq(mrb, str, beg, end-beg));
        mrb_gc_arena_restore(mrb, ai);
        skip = TRUE;
        beg = idx;
        if (lim_p) ++i;
      }
      else {
        end = idx;
      }
    }
  }
  else {                        /* split_type == string */
    mrb_int str_len = RSTRING_LEN(str);
    mrb_int pat_len = RSTRING_LEN(spat);
    mrb_int idx = 0;
    int ai = mrb_gc_arena_save(mrb);

    while (idx < str_len) {
      if (pat_len > 0) {
        end = mrb_memsearch(RSTRING_PTR(spat), pat_len, RSTRING_PTR(str)+idx, str_len - idx);
        if (end < 0) break;
      }
      else {
        end = chars2bytes(str, idx, 1);
      }
      mrb_ary_push(mrb, result, mrb_str_byte_subseq(mrb, str, idx, end));
      mrb_gc_arena_restore(mrb, ai);
      idx += end + pat_len;
      if (lim_p && lim <= ++i) break;
    }
    beg = idx;
  }
  if (RSTRING_LEN(str) > 0 && (lim_p || RSTRING_LEN(str) > beg || lim < 0)) {
    if (RSTRING_LEN(str) == beg) {
      tmp = mrb_str_new_empty(mrb, str);
    }
    else {
      tmp = mrb_str_byte_subseq(mrb, str, beg, RSTRING_LEN(str)-beg);
    }
    mrb_ary_push(mrb, result, tmp);
  }
  if (!lim_p && lim == 0) {
    mrb_int len;
    while ((len = RARRAY_LEN(result)) > 0 &&
           (tmp = RARRAY_PTR(result)[len-1], RSTRING_LEN(tmp) == 0))
      mrb_ary_pop(mrb, result);
  }

  return result;
}

mrb_value
mrb_str_len_to_inum(mrb_state *mrb, const char *str, mrb_int len, mrb_int base, int badcheck)
{
  const char *p = str;
  const char *pend = str + len;
  char sign = 1;
  int c;
  uint64_t n = 0;
  mrb_int val;

#define conv_digit(c) \
    (ISDIGIT(c) ? ((c) - '0') : \
     ISLOWER(c) ? ((c) - 'a' + 10) : \
     ISUPPER(c) ? ((c) - 'A' + 10) : \
     -1)

  if (!p) {
    if (badcheck) goto bad;
    return mrb_fixnum_value(0);
  }
  while (p<pend && ISSPACE(*p))
    p++;

  if (p[0] == '+') {
    p++;
  }
  else if (p[0] == '-') {
    p++;
    sign = 0;
  }
  if (base <= 0) {
    if (p[0] == '0') {
      switch (p[1]) {
        case 'x': case 'X':
          base = 16;
          break;
        case 'b': case 'B':
          base = 2;
          break;
        case 'o': case 'O':
          base = 8;
          break;
        case 'd': case 'D':
          base = 10;
          break;
        default:
          base = 8;
          break;
      }
    }
    else if (base < -1) {
      base = -base;
    }
    else {
      base = 10;
    }
  }
  switch (base) {
    case 2:
      if (p[0] == '0' && (p[1] == 'b'||p[1] == 'B')) {
        p += 2;
      }
      break;
    case 3:
      break;
    case 8:
      if (p[0] == '0' && (p[1] == 'o'||p[1] == 'O')) {
        p += 2;
      }
    case 4: case 5: case 6: case 7:
      break;
    case 10:
      if (p[0] == '0' && (p[1] == 'd'||p[1] == 'D')) {
        p += 2;
      }
    case 9: case 11: case 12: case 13: case 14: case 15:
      break;
    case 16:
      if (p[0] == '0' && (p[1] == 'x'||p[1] == 'X')) {
        p += 2;
      }
      break;
    default:
      if (base < 2 || 36 < base) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "illegal radix %i", base);
      }
      break;
  } /* end of switch (base) { */
  if (p>=pend) {
    if (badcheck) goto bad;
    return mrb_fixnum_value(0);
  }
  if (*p == '0') {    /* squeeze preceding 0s */
    p++;
    while (p<pend) {
      c = *p++;
      if (c == '_') {
        if (p<pend && *p == '_') {
          if (badcheck) goto bad;
          break;
        }
        continue;
      }
      if (c != '0') {
        p--;
        break;
      }
    }
    if (*(p - 1) == '0')
      p--;
  }
  if (p == pend || *p == '_') {
    if (badcheck) goto bad;
    return mrb_fixnum_value(0);
  }
  for ( ;p<pend;p++) {
    if (*p == '_') {
      p++;
      if (p==pend) {
        if (badcheck) goto bad;
        continue;
      }
      if (*p == '_') {
        if (badcheck) goto bad;
        break;
      }
    }
    if (badcheck && *p == '\0') {
      goto nullbyte;
    }
    c = conv_digit(*p);
    if (c < 0 || c >= base) {
      break;
    }
    n *= base;
    n += c;
    if (n > (uint64_t)MRB_INT_MAX + (sign ? 0 : 1)) {
#ifndef MRB_WITHOUT_FLOAT
      if (base == 10) {
        return mrb_float_value(mrb, mrb_str_to_dbl(mrb, mrb_str_new(mrb, str, len), badcheck));
      }
      else
#endif
      {
        mrb_raisef(mrb, E_RANGE_ERROR, "string (%l) too big for integer", str, pend-str);
      }
    }
  }
  val = (mrb_int)n;
  if (badcheck) {
    if (p == str) goto bad;             /* no number */
    if (*(p - 1) == '_') goto bad;      /* trailing '_' */
    while (p<pend && ISSPACE(*p)) p++;
    if (p<pend) goto bad;               /* trailing garbage */
  }

  return mrb_fixnum_value(sign ? val : -val);
 nullbyte:
  mrb_raise(mrb, E_ARGUMENT_ERROR, "string contains null byte");
  /* not reached */
 bad:
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid string for number(%!l)", str, pend-str);
  /* not reached */
  return mrb_fixnum_value(0);
}

MRB_API mrb_value
mrb_cstr_to_inum(mrb_state *mrb, const char *str, mrb_int base, mrb_bool badcheck)
{
  return mrb_str_len_to_inum(mrb, str, strlen(str), base, badcheck);
}

/* obslete: use RSTRING_CSTR() or mrb_string_cstr() */
MRB_API const char*
mrb_string_value_cstr(mrb_state *mrb, mrb_value *ptr)
{
  struct RString *ps;
  const char *p;
  mrb_int len;

  check_null_byte(mrb, *ptr);
  ps = mrb_str_ptr(*ptr);
  p = RSTR_PTR(ps);
  len = RSTR_LEN(ps);
  if (p[len] == '\0') {
    return p;
  }

  /*
   * Even after str_modify_keep_ascii(), NULL termination is not ensured if
   * RSTR_SET_LEN() is used explicitly (e.g. String#delete_suffix!).
   */
  str_modify_keep_ascii(mrb, ps);
  RSTR_PTR(ps)[len] = '\0';
  return RSTR_PTR(ps);
}

MRB_API const char*
mrb_string_cstr(mrb_state *mrb, mrb_value str)
{
  return mrb_string_value_cstr(mrb, &str);
}

MRB_API mrb_value
mrb_str_to_inum(mrb_state *mrb, mrb_value str, mrb_int base, mrb_bool badcheck)
{
  const char *s;
  mrb_int len;

  mrb_to_str(mrb, str);
  s = RSTRING_PTR(str);
  len = RSTRING_LEN(str);
  return mrb_str_len_to_inum(mrb, s, len, base, badcheck);
}

/* 15.2.10.5.38 */
/*
 *  call-seq:
 *     str.to_i(base=10)   => integer
 *
 *  Returns the result of interpreting leading characters in <i>str</i> as an
 *  integer base <i>base</i> (between 2 and 36). Extraneous characters past the
 *  end of a valid number are ignored. If there is not a valid number at the
 *  start of <i>str</i>, <code>0</code> is returned. This method never raises an
 *  exception.
 *
 *     "12345".to_i             #=> 12345
 *     "99 red balloons".to_i   #=> 99
 *     "0a".to_i                #=> 0
 *     "0a".to_i(16)            #=> 10
 *     "hello".to_i             #=> 0
 *     "1100101".to_i(2)        #=> 101
 *     "1100101".to_i(8)        #=> 294977
 *     "1100101".to_i(10)       #=> 1100101
 *     "1100101".to_i(16)       #=> 17826049
 */
static mrb_value
mrb_str_to_i(mrb_state *mrb, mrb_value self)
{
  mrb_int base = 10;

  mrb_get_args(mrb, "|i", &base);
  if (base < 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "illegal radix %i", base);
  }
  return mrb_str_to_inum(mrb, self, base, FALSE);
}

#ifndef MRB_WITHOUT_FLOAT
double
mrb_str_len_to_dbl(mrb_state *mrb, const char *s, size_t len, mrb_bool badcheck)
{
  char buf[DBL_DIG * 4 + 20];
  const char *p = s, *p2;
  const char *pend = p + len;
  char *end;
  char *n;
  char prev = 0;
  double d;
  mrb_bool dot = FALSE;

  if (!p) return 0.0;
  while (p<pend && ISSPACE(*p)) p++;
  p2 = p;

  if (pend - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    mrb_value x;

    if (!badcheck) return 0.0;
    x = mrb_str_len_to_inum(mrb, p, pend-p, 0, badcheck);
    if (mrb_fixnum_p(x))
      d = (double)mrb_fixnum(x);
    else /* if (mrb_float_p(x)) */
      d = mrb_float(x);
    return d;
  }
  while (p < pend) {
    if (!*p) {
      if (badcheck) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "string for Float contains null byte");
        /* not reached */
      }
      pend = p;
      p = p2;
      goto nocopy;
    }
    if (!badcheck && *p == ' ') {
      pend = p;
      p = p2;
      goto nocopy;
    }
    if (*p == '_') break;
    p++;
  }
  p = p2;
  n = buf;
  while (p < pend) {
    char c = *p++;
    if (c == '.') dot = TRUE;
    if (c == '_') {
      /* remove an underscore between digits */
      if (n == buf || !ISDIGIT(prev) || p == pend) {
        if (badcheck) goto bad;
        break;
      }
    }
    else if (badcheck && prev == '_' && !ISDIGIT(c)) goto bad;
    else {
      const char *bend = buf+sizeof(buf)-1;
      if (n==bend) {            /* buffer overflow */
        if (dot) break;         /* cut off remaining fractions */
        return INFINITY;
      }
      *n++ = c;
    }
    prev = c;
  }
  *n = '\0';
  p = buf;
  pend = n;
nocopy:
  d = mrb_float_read(p, &end);
  if (p == end) {
    if (badcheck) {
bad:
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid string for float(%!s)", s);
      /* not reached */
    }
    return d;
  }
  if (badcheck) {
    if (!end || p == end) goto bad;
    while (end<pend && ISSPACE(*end)) end++;
    if (end<pend) goto bad;
  }
  return d;
}

MRB_API double
mrb_cstr_to_dbl(mrb_state *mrb, const char *s, mrb_bool badcheck)
{
  return mrb_str_len_to_dbl(mrb, s, strlen(s), badcheck);
}

MRB_API double
mrb_str_to_dbl(mrb_state *mrb, mrb_value str, mrb_bool badcheck)
{
  return mrb_str_len_to_dbl(mrb, RSTRING_PTR(str), RSTRING_LEN(str), badcheck);
}

/* 15.2.10.5.39 */
/*
 *  call-seq:
 *     str.to_f   => float
 *
 *  Returns the result of interpreting leading characters in <i>str</i> as a
 *  floating point number. Extraneous characters past the end of a valid number
 *  are ignored. If there is not a valid number at the start of <i>str</i>,
 *  <code>0.0</code> is returned. This method never raises an exception.
 *
 *     "123.45e1".to_f        #=> 1234.5
 *     "45.67 degrees".to_f   #=> 45.67
 *     "thx1138".to_f         #=> 0.0
 */
static mrb_value
mrb_str_to_f(mrb_state *mrb, mrb_value self)
{
  return mrb_float_value(mrb, mrb_str_to_dbl(mrb, self, FALSE));
}
#endif

/* 15.2.10.5.40 */
/*
 *  call-seq:
 *     str.to_s     => str
 *
 *  Returns the receiver.
 */
static mrb_value
mrb_str_to_s(mrb_state *mrb, mrb_value self)
{
  if (mrb_obj_class(mrb, self) != mrb->string_class) {
    return mrb_str_dup(mrb, self);
  }
  return self;
}

/* 15.2.10.5.43 */
/*
 *  call-seq:
 *     str.upcase!   => str or nil
 *
 *  Upcases the contents of <i>str</i>, returning <code>nil</code> if no changes
 *  were made.
 */
static mrb_value
mrb_str_upcase_bang(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  char *p, *pend;
  mrb_bool modify = FALSE;

  mrb_str_modify_keep_ascii(mrb, s);
  p = RSTRING_PTR(str);
  pend = RSTRING_END(str);
  while (p < pend) {
    if (ISLOWER(*p)) {
      *p = TOUPPER(*p);
      modify = TRUE;
    }
    p++;
  }

  if (modify) return str;
  return mrb_nil_value();
}

/* 15.2.10.5.42 */
/*
 *  call-seq:
 *     str.upcase   => new_str
 *
 *  Returns a copy of <i>str</i> with all lowercase letters replaced with their
 *  uppercase counterparts. The operation is locale insensitive---only
 *  characters 'a' to 'z' are affected.
 *
 *     "hEllO".upcase   #=> "HELLO"
 */
static mrb_value
mrb_str_upcase(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  str = mrb_str_dup(mrb, self);
  mrb_str_upcase_bang(mrb, str);
  return str;
}

/*
 *  call-seq:
 *     str.dump   -> new_str
 *
 *  Produces a version of <i>str</i> with all nonprinting characters replaced by
 *  <code>\nnn</code> notation and all special characters escaped.
 */
mrb_value
mrb_str_dump(mrb_state *mrb, mrb_value str)
{
  return str_escape(mrb, str, FALSE);
}

MRB_API mrb_value
mrb_str_cat(mrb_state *mrb, mrb_value str, const char *ptr, size_t len)
{
  struct RString *s = mrb_str_ptr(str);
  size_t capa;
  size_t total;
  ptrdiff_t off = -1;

  if (len == 0) return str;
  mrb_str_modify(mrb, s);
  if (ptr >= RSTR_PTR(s) && ptr <= RSTR_PTR(s) + (size_t)RSTR_LEN(s)) {
      off = ptr - RSTR_PTR(s);
  }

  capa = RSTR_CAPA(s);
  total = RSTR_LEN(s)+len;
  if (total >= MRB_SSIZE_MAX) {
  size_error:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string size too big");
  }
  if (capa <= total) {
    if (capa == 0) capa = 1;
    while (capa <= total) {
      if (capa <= MRB_SSIZE_MAX / 2) {
        capa *= 2;
      }
      else {
        capa = total+1;
      }
    }
    if (capa <= total || capa > MRB_SSIZE_MAX) {
      goto size_error;
    }
    resize_capa(mrb, s, capa);
  }
  if (off != -1) {
      ptr = RSTR_PTR(s) + off;
  }
  memcpy(RSTR_PTR(s) + RSTR_LEN(s), ptr, len);
  mrb_assert_int_fit(size_t, total, mrb_ssize, MRB_SSIZE_MAX);
  RSTR_SET_LEN(s, total);
  RSTR_PTR(s)[total] = '\0';   /* sentinel */
  return str;
}

MRB_API mrb_value
mrb_str_cat_cstr(mrb_state *mrb, mrb_value str, const char *ptr)
{
  return mrb_str_cat(mrb, str, ptr, ptr ? strlen(ptr) : 0);
}

MRB_API mrb_value
mrb_str_cat_str(mrb_state *mrb, mrb_value str, mrb_value str2)
{
  if (mrb_str_ptr(str) == mrb_str_ptr(str2)) {
    mrb_str_modify(mrb, mrb_str_ptr(str));
  }
  return mrb_str_cat(mrb, str, RSTRING_PTR(str2), RSTRING_LEN(str2));
}

MRB_API mrb_value
mrb_str_append(mrb_state *mrb, mrb_value str1, mrb_value str2)
{
  mrb_to_str(mrb, str2);
  return mrb_str_cat_str(mrb, str1, str2);
}

/*
 * call-seq:
 *   str.inspect   -> string
 *
 * Returns a printable version of _str_, surrounded by quote marks,
 * with special characters escaped.
 *
 *    str = "hello"
 *    str[3] = "\b"
 *    str.inspect       #=> "\"hel\\bo\""
 */
mrb_value
mrb_str_inspect(mrb_state *mrb, mrb_value str)
{
  return str_escape(mrb, str, TRUE);
}

/*
 * call-seq:
 *   str.bytes   -> array of fixnums
 *
 * Returns an array of bytes in _str_.
 *
 *    str = "hello"
 *    str.bytes       #=> [104, 101, 108, 108, 111]
 */
static mrb_value
mrb_str_bytes(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  mrb_value a = mrb_ary_new_capa(mrb, RSTR_LEN(s));
  unsigned char *p = (unsigned char *)(RSTR_PTR(s)), *pend = p + RSTR_LEN(s);

  while (p < pend) {
    mrb_ary_push(mrb, a, mrb_fixnum_value(p[0]));
    p++;
  }
  return a;
}

/*
 *  call-seq:
 *     str.getbyte(index)          -> 0 .. 255
 *
 *  returns the <i>index</i>th byte as an integer.
 */
static mrb_value
mrb_str_getbyte(mrb_state *mrb, mrb_value str)
{
  mrb_int pos;
  mrb_get_args(mrb, "i", &pos);

  if (pos < 0)
    pos += RSTRING_LEN(str);
  if (pos < 0 ||  RSTRING_LEN(str) <= pos)
    return mrb_nil_value();

  return mrb_fixnum_value((unsigned char)RSTRING_PTR(str)[pos]);
}

/*
 *  call-seq:
 *     str.setbyte(index, integer) -> integer
 *
 *  modifies the <i>index</i>th byte as <i>integer</i>.
 */
static mrb_value
mrb_str_setbyte(mrb_state *mrb, mrb_value str)
{
  mrb_int pos, byte;
  mrb_int len;

  mrb_get_args(mrb, "ii", &pos, &byte);

  len = RSTRING_LEN(str);
  if (pos < -len || len <= pos)
    mrb_raisef(mrb, E_INDEX_ERROR, "index %i out of string", pos);
  if (pos < 0)
    pos += len;

  mrb_str_modify(mrb, mrb_str_ptr(str));
  byte &= 0xff;
  RSTRING_PTR(str)[pos] = (unsigned char)byte;
  return mrb_fixnum_value((unsigned char)byte);
}

/*
 *  call-seq:
 *     str.byteslice(integer)           -> new_str or nil
 *     str.byteslice(integer, integer)   -> new_str or nil
 *     str.byteslice(range)            -> new_str or nil
 *
 *  Byte Reference---If passed a single Integer, returns a
 *  substring of one byte at that position. If passed two Integer
 *  objects, returns a substring starting at the offset given by the first, and
 *  a length given by the second. If given a Range, a substring containing
 *  bytes at offsets given by the range is returned. In all three cases, if
 *  an offset is negative, it is counted from the end of <i>str</i>. Returns
 *  <code>nil</code> if the initial offset falls outside the string, the length
 *  is negative, or the beginning of the range is greater than the end.
 *  The encoding of the resulted string keeps original encoding.
 *
 *     "hello".byteslice(1)     #=> "e"
 *     "hello".byteslice(-1)    #=> "o"
 *     "hello".byteslice(1, 2)  #=> "el"
 *     "\x80\u3042".byteslice(1, 3) #=> "\u3042"
 *     "\x03\u3042\xff".byteslice(1..3) #=> "\u3042"
 */
static mrb_value
mrb_str_byteslice(mrb_state *mrb, mrb_value str)
{
  mrb_value a1, a2;
  mrb_int str_len = RSTRING_LEN(str), beg, len;
  mrb_bool empty = TRUE;

  if (mrb_get_args(mrb, "o|o", &a1, &a2) == 2) {
    beg = mrb_fixnum(mrb_to_int(mrb, a1));
    len = mrb_fixnum(mrb_to_int(mrb, a2));
  }
  else if (mrb_range_p(a1)) {
    if (mrb_range_beg_len(mrb, a1, &beg, &len, str_len, TRUE) != MRB_RANGE_OK) {
      return mrb_nil_value();
    }
  }
  else {
    beg = mrb_fixnum(mrb_to_int(mrb, a1));
    len = 1;
    empty = FALSE;
  }

  if (mrb_str_beg_len(str_len, &beg, &len) && (empty || len != 0)) {
    return mrb_str_byte_subseq(mrb, str, beg, len);
  }
  else {
    return mrb_nil_value();
  }
}

/* ---------------------------*/
void
mrb_init_string(mrb_state *mrb)
{
  struct RClass *s;

  mrb_static_assert(RSTRING_EMBED_LEN_MAX < (1 << MRB_STR_EMBED_LEN_BIT),
                    "pointer size too big for embedded string");

  mrb->string_class = s = mrb_define_class(mrb, "String", mrb->object_class);             /* 15.2.10 */
  MRB_SET_INSTANCE_TT(s, MRB_TT_STRING);

  mrb_define_method(mrb, s, "bytesize",        mrb_str_bytesize,        MRB_ARGS_NONE());

  mrb_define_method(mrb, s, "<=>",             mrb_str_cmp_m,           MRB_ARGS_REQ(1)); /* 15.2.10.5.1  */
  mrb_define_method(mrb, s, "==",              mrb_str_equal_m,         MRB_ARGS_REQ(1)); /* 15.2.10.5.2  */
  mrb_define_method(mrb, s, "+",               mrb_str_plus_m,          MRB_ARGS_REQ(1)); /* 15.2.10.5.4  */
  mrb_define_method(mrb, s, "*",               mrb_str_times,           MRB_ARGS_REQ(1)); /* 15.2.10.5.5  */
  mrb_define_method(mrb, s, "[]",              mrb_str_aref_m,          MRB_ARGS_ANY());  /* 15.2.10.5.6  */
  mrb_define_method(mrb, s, "[]=",             mrb_str_aset_m,          MRB_ARGS_ANY());
  mrb_define_method(mrb, s, "capitalize",      mrb_str_capitalize,      MRB_ARGS_NONE()); /* 15.2.10.5.7  */
  mrb_define_method(mrb, s, "capitalize!",     mrb_str_capitalize_bang, MRB_ARGS_NONE()); /* 15.2.10.5.8  */
  mrb_define_method(mrb, s, "chomp",           mrb_str_chomp,           MRB_ARGS_ANY());  /* 15.2.10.5.9  */
  mrb_define_method(mrb, s, "chomp!",          mrb_str_chomp_bang,      MRB_ARGS_ANY());  /* 15.2.10.5.10 */
  mrb_define_method(mrb, s, "chop",            mrb_str_chop,            MRB_ARGS_NONE()); /* 15.2.10.5.11 */
  mrb_define_method(mrb, s, "chop!",           mrb_str_chop_bang,       MRB_ARGS_NONE()); /* 15.2.10.5.12 */
  mrb_define_method(mrb, s, "downcase",        mrb_str_downcase,        MRB_ARGS_NONE()); /* 15.2.10.5.13 */
  mrb_define_method(mrb, s, "downcase!",       mrb_str_downcase_bang,   MRB_ARGS_NONE()); /* 15.2.10.5.14 */
  mrb_define_method(mrb, s, "empty?",          mrb_str_empty_p,         MRB_ARGS_NONE()); /* 15.2.10.5.16 */
  mrb_define_method(mrb, s, "eql?",            mrb_str_eql,             MRB_ARGS_REQ(1)); /* 15.2.10.5.17 */

  mrb_define_method(mrb, s, "hash",            mrb_str_hash_m,          MRB_ARGS_NONE()); /* 15.2.10.5.20 */
  mrb_define_method(mrb, s, "include?",        mrb_str_include,         MRB_ARGS_REQ(1)); /* 15.2.10.5.21 */
  mrb_define_method(mrb, s, "index",           mrb_str_index_m,         MRB_ARGS_ARG(1,1));  /* 15.2.10.5.22 */
  mrb_define_method(mrb, s, "initialize",      mrb_str_init,            MRB_ARGS_REQ(1)); /* 15.2.10.5.23 */
  mrb_define_method(mrb, s, "initialize_copy", mrb_str_replace,         MRB_ARGS_REQ(1)); /* 15.2.10.5.24 */
  mrb_define_method(mrb, s, "intern",          mrb_str_intern,          MRB_ARGS_NONE()); /* 15.2.10.5.25 */
  mrb_define_method(mrb, s, "length",          mrb_str_size,            MRB_ARGS_NONE()); /* 15.2.10.5.26 */
  mrb_define_method(mrb, s, "replace",         mrb_str_replace,         MRB_ARGS_REQ(1)); /* 15.2.10.5.28 */
  mrb_define_method(mrb, s, "reverse",         mrb_str_reverse,         MRB_ARGS_NONE()); /* 15.2.10.5.29 */
  mrb_define_method(mrb, s, "reverse!",        mrb_str_reverse_bang,    MRB_ARGS_NONE()); /* 15.2.10.5.30 */
  mrb_define_method(mrb, s, "rindex",          mrb_str_rindex,          MRB_ARGS_ANY());  /* 15.2.10.5.31 */
  mrb_define_method(mrb, s, "size",            mrb_str_size,            MRB_ARGS_NONE()); /* 15.2.10.5.33 */
  mrb_define_method(mrb, s, "slice",           mrb_str_aref_m,          MRB_ARGS_ANY());  /* 15.2.10.5.34 */
  mrb_define_method(mrb, s, "split",           mrb_str_split_m,         MRB_ARGS_ANY());  /* 15.2.10.5.35 */

#ifndef MRB_WITHOUT_FLOAT
  mrb_define_method(mrb, s, "to_f",            mrb_str_to_f,            MRB_ARGS_NONE()); /* 15.2.10.5.38 */
#endif
  mrb_define_method(mrb, s, "to_i",            mrb_str_to_i,            MRB_ARGS_ANY());  /* 15.2.10.5.39 */
  mrb_define_method(mrb, s, "to_s",            mrb_str_to_s,            MRB_ARGS_NONE()); /* 15.2.10.5.40 */
  mrb_define_method(mrb, s, "to_str",          mrb_str_to_s,            MRB_ARGS_NONE());
  mrb_define_method(mrb, s, "to_sym",          mrb_str_intern,          MRB_ARGS_NONE()); /* 15.2.10.5.41 */
  mrb_define_method(mrb, s, "upcase",          mrb_str_upcase,          MRB_ARGS_NONE()); /* 15.2.10.5.42 */
  mrb_define_method(mrb, s, "upcase!",         mrb_str_upcase_bang,     MRB_ARGS_NONE()); /* 15.2.10.5.43 */
  mrb_define_method(mrb, s, "inspect",         mrb_str_inspect,         MRB_ARGS_NONE()); /* 15.2.10.5.46(x) */
  mrb_define_method(mrb, s, "bytes",           mrb_str_bytes,           MRB_ARGS_NONE());

  mrb_define_method(mrb, s, "getbyte",         mrb_str_getbyte,         MRB_ARGS_REQ(1));
  mrb_define_method(mrb, s, "setbyte",         mrb_str_setbyte,         MRB_ARGS_REQ(2));
  mrb_define_method(mrb, s, "byteslice",       mrb_str_byteslice,       MRB_ARGS_ARG(1,1));
}

#ifndef MRB_WITHOUT_FLOAT
/*
 * Source code for the "strtod" library procedure.
 *
 * Copyright (c) 1988-1993 The Regents of the University of California.
 * Copyright (c) 1994 Sun Microsystems, Inc.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 * RCS: @(#) $Id: strtod.c 11708 2007-02-12 23:01:19Z shyouhei $
 */

#include <ctype.h>
#include <errno.h>

static const int maxExponent = 511; /* Largest possible base 10 exponent.  Any
                                     * exponent larger than this will already
                                     * produce underflow or overflow, so there's
                                     * no need to worry about additional digits.
                                     */
static const double powersOf10[] = {/* Table giving binary powers of 10.  Entry */
    10.,                            /* is 10^2^i.  Used to convert decimal */
    100.,                           /* exponents into floating-point numbers. */
    1.0e4,
    1.0e8,
    1.0e16,
    1.0e32,
    1.0e64,
    1.0e128,
    1.0e256
};

MRB_API double
mrb_float_read(const char *string, char **endPtr)
/*  const char *string;            A decimal ASCII floating-point number,
                                 * optionally preceded by white space.
                                 * Must have form "-I.FE-X", where I is the
                                 * integer part of the mantissa, F is the
                                 * fractional part of the mantissa, and X
                                 * is the exponent.  Either of the signs
                                 * may be "+", "-", or omitted.  Either I
                                 * or F may be omitted, or both.  The decimal
                                 * point isn't necessary unless F is present.
                                 * The "E" may actually be an "e".  E and X
                                 * may both be omitted (but not just one).
                                 */
/*  char **endPtr;                 If non-NULL, store terminating character's
                                 * address here. */
{
    int sign, expSign = FALSE;
    double fraction, dblExp;
    const double *d;
    const char *p;
    int c;
    int exp = 0;                /* Exponent read from "EX" field. */
    int fracExp = 0;            /* Exponent that derives from the fractional
                                 * part.  Under normal circumstatnces, it is
                                 * the negative of the number of digits in F.
                                 * However, if I is very long, the last digits
                                 * of I get dropped (otherwise a long I with a
                                 * large negative exponent could cause an
                                 * unnecessary overflow on I alone).  In this
                                 * case, fracExp is incremented one for each
                                 * dropped digit. */
    int mantSize;               /* Number of digits in mantissa. */
    int decPt;                  /* Number of mantissa digits BEFORE decimal
                                 * point. */
    const char *pExp;           /* Temporarily holds location of exponent
                                 * in string. */

    /*
     * Strip off leading blanks and check for a sign.
     */

    p = string;
    while (ISSPACE(*p)) {
      p += 1;
    }
    if (*p == '-') {
      sign = TRUE;
      p += 1;
    }
    else {
      if (*p == '+') {
        p += 1;
      }
      sign = FALSE;
    }

    /*
     * Count the number of digits in the mantissa (including the decimal
     * point), and also locate the decimal point.
     */

    decPt = -1;
    for (mantSize = 0; ; mantSize += 1)
    {
      c = *p;
      if (!ISDIGIT(c)) {
        if ((c != '.') || (decPt >= 0)) {
          break;
        }
        decPt = mantSize;
      }
      p += 1;
    }

    /*
     * Now suck up the digits in the mantissa.  Use two integers to
     * collect 9 digits each (this is faster than using floating-point).
     * If the mantissa has more than 18 digits, ignore the extras, since
     * they can't affect the value anyway.
     */

    pExp  = p;
    p -= mantSize;
    if (decPt < 0) {
      decPt = mantSize;
    }
    else {
      mantSize -= 1; /* One of the digits was the point. */
    }
    if (mantSize > 18) {
      if (decPt - 18 > 29999) {
        fracExp = 29999;
      }
      else {
        fracExp = decPt - 18;
      }
      mantSize = 18;
    }
    else {
      fracExp = decPt - mantSize;
    }
    if (mantSize == 0) {
      fraction = 0.0;
      p = string;
      goto done;
    }
    else {
      int frac1, frac2;
      frac1 = 0;
      for ( ; mantSize > 9; mantSize -= 1)
      {
        c = *p;
        p += 1;
        if (c == '.') {
          c = *p;
          p += 1;
        }
        frac1 = 10*frac1 + (c - '0');
      }
      frac2 = 0;
      for (; mantSize > 0; mantSize -= 1)
      {
        c = *p;
        p += 1;
        if (c == '.') {
          c = *p;
          p += 1;
        }
        frac2 = 10*frac2 + (c - '0');
      }
      fraction = (1.0e9 * frac1) + frac2;
    }

    /*
     * Skim off the exponent.
     */

    p = pExp;
    if ((*p == 'E') || (*p == 'e')) {
      p += 1;
      if (*p == '-') {
        expSign = TRUE;
        p += 1;
      }
      else {
        if (*p == '+') {
          p += 1;
        }
        expSign = FALSE;
      }
      while (ISDIGIT(*p)) {
        exp = exp * 10 + (*p - '0');
        if (exp > 19999) {
          exp = 19999;
        }
        p += 1;
      }
    }
    if (expSign) {
      exp = fracExp - exp;
    }
    else {
      exp = fracExp + exp;
    }

    /*
     * Generate a floating-point number that represents the exponent.
     * Do this by processing the exponent one bit at a time to combine
     * many powers of 2 of 10. Then combine the exponent with the
     * fraction.
     */

    if (exp < 0) {
      expSign = TRUE;
      exp = -exp;
    }
    else {
      expSign = FALSE;
    }
    if (exp > maxExponent) {
      exp = maxExponent;
      errno = ERANGE;
    }
    dblExp = 1.0;
    for (d = powersOf10; exp != 0; exp >>= 1, d += 1) {
      if (exp & 01) {
        dblExp *= *d;
      }
    }
    if (expSign) {
      fraction /= dblExp;
    }
    else {
      fraction *= dblExp;
    }

done:
    if (endPtr != NULL) {
      *endPtr = (char *) p;
    }

    if (sign) {
      return -fraction;
    }
    return fraction;
}
#endif
/*
** symbol.c - Symbol class
**
** See Copyright Notice in mruby.h
*/

#include <limits.h>
#include <string.h>
#include <mruby.h>
#include <mruby/khash.h>
#include <mruby/string.h>
#include <mruby/dump.h>
#include <mruby/class.h>

/* ------------------------------------------------------ */
typedef struct symbol_name {
  mrb_bool lit : 1;
  uint8_t prev;
  uint16_t len;
  const char *name;
} symbol_name;

#define SYMBOL_INLINE_BIT_POS       1
#define SYMBOL_INLINE_LOWER_BIT_POS 2
#define SYMBOL_INLINE               (1 << (SYMBOL_INLINE_BIT_POS - 1))
#define SYMBOL_INLINE_LOWER         (1 << (SYMBOL_INLINE_LOWER_BIT_POS - 1))
#define SYMBOL_NORMAL_SHIFT         SYMBOL_INLINE_BIT_POS
#define SYMBOL_INLINE_SHIFT         SYMBOL_INLINE_LOWER_BIT_POS
#ifdef MRB_ENABLE_ALL_SYMBOLS
# define SYMBOL_INLINE_P(sym) FALSE
# define SYMBOL_INLINE_LOWER_P(sym) FALSE
# define sym_inline_pack(name, len) 0
# define sym_inline_unpack(sym, buf, lenp) NULL
#else
# define SYMBOL_INLINE_P(sym) ((sym) & SYMBOL_INLINE)
# define SYMBOL_INLINE_LOWER_P(sym) ((sym) & SYMBOL_INLINE_LOWER)
#endif

static void
sym_validate_len(mrb_state *mrb, size_t len)
{
  if (len >= RITE_LV_NULL_MARK) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "symbol length too long");
  }
}

#ifndef MRB_ENABLE_ALL_SYMBOLS
static const char pack_table[] = "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static mrb_sym
sym_inline_pack(const char *name, size_t len)
{
  const size_t lower_length_max = (MRB_SYMBOL_BIT - 2) / 5;
  const size_t mix_length_max   = (MRB_SYMBOL_BIT - 2) / 6;

  char c;
  const char *p;
  size_t i;
  mrb_sym sym = 0;
  mrb_bool lower = TRUE;

  if (len > lower_length_max) return 0; /* too long */
  for (i=0; i<len; i++) {
    uint32_t bits;

    c = name[i];
    if (c == 0) return 0;       /* NUL in name */
    p = strchr(pack_table, (int)c);
    if (p == 0) return 0;       /* non alnum char */
    bits = (uint32_t)(p - pack_table)+1;
    if (bits > 27) lower = FALSE;
    if (i >= mix_length_max) break;
    sym |= bits<<(i*6+SYMBOL_INLINE_SHIFT);
  }
  if (lower) {
    sym = 0;
    for (i=0; i<len; i++) {
      uint32_t bits;

      c = name[i];
      p = strchr(pack_table, (int)c);
      bits = (uint32_t)(p - pack_table)+1;
      sym |= bits<<(i*5+SYMBOL_INLINE_SHIFT);
    }
    return sym | SYMBOL_INLINE | SYMBOL_INLINE_LOWER;
  }
  if (len > mix_length_max) return 0;
  return sym | SYMBOL_INLINE;
}

static const char*
sym_inline_unpack(mrb_sym sym, char *buf, mrb_int *lenp)
{
  int bit_per_char = SYMBOL_INLINE_LOWER_P(sym) ? 5 : 6;
  int i;

  mrb_assert(SYMBOL_INLINE_P(sym));

  for (i=0; i<30/bit_per_char; i++) {
    uint32_t bits = sym>>(i*bit_per_char+SYMBOL_INLINE_SHIFT) & ((1<<bit_per_char)-1);
    if (bits == 0) break;
    buf[i] = pack_table[bits-1];;
  }
  buf[i] = '\0';
  if (lenp) *lenp = i;
  return buf;
}
#endif

static uint8_t
symhash(const char *key, size_t len)
{
    uint32_t hash, i;

    for(hash = i = 0; i < len; ++i) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash & 0xff;
}

static mrb_sym
find_symbol(mrb_state *mrb, const char *name, size_t len, uint8_t *hashp)
{
  mrb_sym i;
  symbol_name *sname;
  uint8_t hash;

  /* inline symbol */
  i = sym_inline_pack(name, len);
  if (i > 0) return i;

  hash = symhash(name, len);
  if (hashp) *hashp = hash;

  i = mrb->symhash[hash];
  if (i == 0) return 0;
  do {
    sname = &mrb->symtbl[i];
    if (sname->len == len && memcmp(sname->name, name, len) == 0) {
      return i<<SYMBOL_NORMAL_SHIFT;
    }
    if (sname->prev == 0xff) {
      i -= 0xff;
      sname = &mrb->symtbl[i];
      while (mrb->symtbl < sname) {
        if (sname->len == len && memcmp(sname->name, name, len) == 0) {
          return (mrb_sym)(sname - mrb->symtbl)<<SYMBOL_NORMAL_SHIFT;
        }
        sname--;
      }
      return 0;
    }
    i -= sname->prev;
  } while (sname->prev > 0);
  return 0;
}

static mrb_sym
sym_intern(mrb_state *mrb, const char *name, size_t len, mrb_bool lit)
{
  mrb_sym sym;
  symbol_name *sname;
  uint8_t hash;

  sym_validate_len(mrb, len);
  sym = find_symbol(mrb, name, len, &hash);
  if (sym > 0) return sym;

  /* registering a new symbol */
  sym = mrb->symidx + 1;
  if (mrb->symcapa < sym) {
    size_t symcapa = mrb->symcapa;
    if (symcapa == 0) symcapa = 100;
    else symcapa = (size_t)(symcapa * 6 / 5);
    mrb->symtbl = (symbol_name*)mrb_realloc(mrb, mrb->symtbl, sizeof(symbol_name)*(symcapa+1));
    mrb->symcapa = symcapa;
  }
  sname = &mrb->symtbl[sym];
  sname->len = (uint16_t)len;
  if (lit || mrb_ro_data_p(name)) {
    sname->name = name;
    sname->lit = TRUE;
  }
  else {
    char *p = (char *)mrb_malloc(mrb, len+1);
    memcpy(p, name, len);
    p[len] = 0;
    sname->name = (const char*)p;
    sname->lit = FALSE;
  }
  if (mrb->symhash[hash]) {
    mrb_sym i = sym - mrb->symhash[hash];
    if (i > 0xff)
      sname->prev = 0xff;
    else
      sname->prev = i;
  }
  else {
    sname->prev = 0;
  }
  mrb->symhash[hash] = mrb->symidx = sym;

  return sym<<SYMBOL_NORMAL_SHIFT;
}

MRB_API mrb_sym
mrb_intern(mrb_state *mrb, const char *name, size_t len)
{
  return sym_intern(mrb, name, len, FALSE);
}

MRB_API mrb_sym
mrb_intern_static(mrb_state *mrb, const char *name, size_t len)
{
  return sym_intern(mrb, name, len, TRUE);
}

MRB_API mrb_sym
mrb_intern_cstr(mrb_state *mrb, const char *name)
{
  return mrb_intern(mrb, name, strlen(name));
}

MRB_API mrb_sym
mrb_intern_str(mrb_state *mrb, mrb_value str)
{
  return mrb_intern(mrb, RSTRING_PTR(str), RSTRING_LEN(str));
}

MRB_API mrb_value
mrb_check_intern(mrb_state *mrb, const char *name, size_t len)
{
  mrb_sym sym;

  sym_validate_len(mrb, len);
  sym = find_symbol(mrb, name, len, NULL);
  if (sym > 0) return mrb_symbol_value(sym);
  return mrb_nil_value();
}

MRB_API mrb_value
mrb_check_intern_cstr(mrb_state *mrb, const char *name)
{
  return mrb_check_intern(mrb, name, strlen(name));
}

MRB_API mrb_value
mrb_check_intern_str(mrb_state *mrb, mrb_value str)
{
  return mrb_check_intern(mrb, RSTRING_PTR(str), RSTRING_LEN(str));
}

static const char*
sym2name_len(mrb_state *mrb, mrb_sym sym, char *buf, mrb_int *lenp)
{
  if (SYMBOL_INLINE_P(sym)) return sym_inline_unpack(sym, buf, lenp);

  sym >>= SYMBOL_NORMAL_SHIFT;
  if (sym == 0 || mrb->symidx < sym) {
    if (lenp) *lenp = 0;
    return NULL;
  }

  if (lenp) *lenp = mrb->symtbl[sym].len;
  return mrb->symtbl[sym].name;
}

MRB_API const char*
mrb_sym_name_len(mrb_state *mrb, mrb_sym sym, mrb_int *lenp)
{
  return sym2name_len(mrb, sym, mrb->symbuf, lenp);
}

void
mrb_free_symtbl(mrb_state *mrb)
{
  mrb_sym i, lim;

  for (i=1, lim=mrb->symidx+1; i<lim; i++) {
    if (!mrb->symtbl[i].lit) {
      mrb_free(mrb, (char*)mrb->symtbl[i].name);
    }
  }
  mrb_free(mrb, mrb->symtbl);
}

void
mrb_init_symtbl(mrb_state *mrb)
{
}

/**********************************************************************
 * Document-class: Symbol
 *
 *  <code>Symbol</code> objects represent names and some strings
 *  inside the Ruby
 *  interpreter. They are generated using the <code>:name</code> and
 *  <code>:"string"</code> literals
 *  syntax, and by the various <code>to_sym</code> methods. The same
 *  <code>Symbol</code> object will be created for a given name or string
 *  for the duration of a program's execution, regardless of the context
 *  or meaning of that name. Thus if <code>Fred</code> is a constant in
 *  one context, a method in another, and a class in a third, the
 *  <code>Symbol</code> <code>:Fred</code> will be the same object in
 *  all three contexts.
 *
 *     module One
 *       class Fred
 *       end
 *       $f1 = :Fred
 *     end
 *     module Two
 *       Fred = 1
 *       $f2 = :Fred
 *     end
 *     def Fred()
 *     end
 *     $f3 = :Fred
 *     $f1.object_id   #=> 2514190
 *     $f2.object_id   #=> 2514190
 *     $f3.object_id   #=> 2514190
 *
 */

/* 15.2.11.3.2  */
/* 15.2.11.3.3  */
/*
 *  call-seq:
 *     sym.id2name   -> string
 *     sym.to_s      -> string
 *
 *  Returns the name or string corresponding to <i>sym</i>.
 *
 *     :fred.id2name   #=> "fred"
 */
static mrb_value
sym_to_s(mrb_state *mrb, mrb_value sym)
{
  return mrb_sym_str(mrb, mrb_symbol(sym));
}

/* 15.2.11.3.4  */
/*
 * call-seq:
 *   sym.to_sym   -> sym
 *   sym.intern   -> sym
 *
 * In general, <code>to_sym</code> returns the <code>Symbol</code> corresponding
 * to an object. As <i>sym</i> is already a symbol, <code>self</code> is returned
 * in this case.
 */

static mrb_value
sym_to_sym(mrb_state *mrb, mrb_value sym)
{
  return sym;
}

/* 15.2.11.3.5(x)  */
/*
 *  call-seq:
 *     sym.inspect    -> string
 *
 *  Returns the representation of <i>sym</i> as a symbol literal.
 *
 *     :fred.inspect   #=> ":fred"
 */

#if __STDC__
# define SIGN_EXTEND_CHAR(c) ((signed char)(c))
#else  /* not __STDC__ */
/* As in Harbison and Steele.  */
# define SIGN_EXTEND_CHAR(c) ((((unsigned char)(c)) ^ 128) - 128)
#endif
#define is_identchar(c) (SIGN_EXTEND_CHAR(c)!=-1&&(ISALNUM(c) || (c) == '_'))

static mrb_bool
is_special_global_name(const char* m)
{
  switch (*m) {
    case '~': case '*': case '$': case '?': case '!': case '@':
    case '/': case '\\': case ';': case ',': case '.': case '=':
    case ':': case '<': case '>': case '\"':
    case '&': case '`': case '\'': case '+':
    case '0':
      ++m;
      break;
    case '-':
      ++m;
      if (is_identchar(*m)) m += 1;
      break;
    default:
      if (!ISDIGIT(*m)) return FALSE;
      do ++m; while (ISDIGIT(*m));
      break;
  }
  return !*m;
}

static mrb_bool
symname_p(const char *name)
{
  const char *m = name;
  mrb_bool localid = FALSE;

  if (!m) return FALSE;
  switch (*m) {
    case '\0':
      return FALSE;

    case '$':
      if (is_special_global_name(++m)) return TRUE;
      goto id;

    case '@':
      if (*++m == '@') ++m;
      goto id;

    case '<':
      switch (*++m) {
        case '<': ++m; break;
        case '=': if (*++m == '>') ++m; break;
        default: break;
      }
      break;

    case '>':
      switch (*++m) {
        case '>': case '=': ++m; break;
        default: break;
      }
      break;

    case '=':
      switch (*++m) {
        case '~': ++m; break;
        case '=': if (*++m == '=') ++m; break;
        default: return FALSE;
      }
      break;

    case '*':
      if (*++m == '*') ++m;
      break;
    case '!':
      switch (*++m) {
        case '=': case '~': ++m;
      }
      break;
    case '+': case '-':
      if (*++m == '@') ++m;
      break;
    case '|':
      if (*++m == '|') ++m;
      break;
    case '&':
      if (*++m == '&') ++m;
      break;

    case '^': case '/': case '%': case '~': case '`':
      ++m;
      break;

    case '[':
      if (*++m != ']') return FALSE;
      if (*++m == '=') ++m;
      break;

    default:
      localid = !ISUPPER(*m);
id:
      if (*m != '_' && !ISALPHA(*m)) return FALSE;
      while (is_identchar(*m)) m += 1;
      if (localid) {
        switch (*m) {
          case '!': case '?': case '=': ++m;
          default: break;
        }
      }
      break;
  }
  return *m ? FALSE : TRUE;
}

static mrb_value
sym_inspect(mrb_state *mrb, mrb_value sym)
{
  mrb_value str;
  const char *name;
  mrb_int len;
  mrb_sym id = mrb_symbol(sym);
  char *sp;

  name = mrb_sym_name_len(mrb, id, &len);
  str = mrb_str_new(mrb, 0, len+1);
  sp = RSTRING_PTR(str);
  sp[0] = ':';
  memcpy(sp+1, name, len);
  mrb_assert_int_fit(mrb_int, len, size_t, SIZE_MAX);
  if (!symname_p(name) || strlen(name) != (size_t)len) {
    str = mrb_str_inspect(mrb, str);
    sp = RSTRING_PTR(str);
    sp[0] = ':';
    sp[1] = '"';
  }
#ifdef MRB_UTF8_STRING
  if (SYMBOL_INLINE_P(id)) RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
#endif
  return str;
}

MRB_API mrb_value
mrb_sym_str(mrb_state *mrb, mrb_sym sym)
{
  mrb_int len;
  const char *name = mrb_sym_name_len(mrb, sym, &len);

  if (!name) return mrb_undef_value(); /* can't happen */
  if (SYMBOL_INLINE_P(sym)) {
    mrb_value str = mrb_str_new(mrb, name, len);
    RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
    return str;
  }
  return mrb_str_new_static(mrb, name, len);
}

static const char*
sym_name(mrb_state *mrb, mrb_sym sym, mrb_bool dump)
{
  mrb_int len;
  const char *name = mrb_sym_name_len(mrb, sym, &len);

  if (!name) return NULL;
  if (strlen(name) == (size_t)len && (!dump || symname_p(name))) {
    return name;
  }
  else {
    mrb_value str = SYMBOL_INLINE_P(sym) ?
      mrb_str_new(mrb, name, len) : mrb_str_new_static(mrb, name, len);
    str = mrb_str_dump(mrb, str);
    return RSTRING_PTR(str);
  }
}

MRB_API const char*
mrb_sym_name(mrb_state *mrb, mrb_sym sym)
{
  return sym_name(mrb, sym, FALSE);
}

MRB_API const char*
mrb_sym_dump(mrb_state *mrb, mrb_sym sym)
{
  return sym_name(mrb, sym, TRUE);
}

#define lesser(a,b) (((a)>(b))?(b):(a))

static mrb_value
sym_cmp(mrb_state *mrb, mrb_value s1)
{
  mrb_value s2 = mrb_get_arg1(mrb);
  mrb_sym sym1, sym2;

  if (!mrb_symbol_p(s2)) return mrb_nil_value();
  sym1 = mrb_symbol(s1);
  sym2 = mrb_symbol(s2);
  if (sym1 == sym2) return mrb_fixnum_value(0);
  else {
    const char *p1, *p2;
    int retval;
    mrb_int len, len1, len2;
    char buf1[8], buf2[8];

    p1 = sym2name_len(mrb, sym1, buf1, &len1);
    p2 = sym2name_len(mrb, sym2, buf2, &len2);
    len = lesser(len1, len2);
    retval = memcmp(p1, p2, len);
    if (retval == 0) {
      if (len1 == len2) return mrb_fixnum_value(0);
      if (len1 > len2)  return mrb_fixnum_value(1);
      return mrb_fixnum_value(-1);
    }
    if (retval > 0) return mrb_fixnum_value(1);
    return mrb_fixnum_value(-1);
  }
}

void
mrb_init_symbol(mrb_state *mrb)
{
  struct RClass *sym;

  mrb->symbol_class = sym = mrb_define_class(mrb, "Symbol", mrb->object_class);  /* 15.2.11 */
  MRB_SET_INSTANCE_TT(sym, MRB_TT_SYMBOL);
  mrb_undef_class_method(mrb,  sym, "new");

  mrb_define_method(mrb, sym, "id2name", sym_to_s,    MRB_ARGS_NONE());          /* 15.2.11.3.2 */
  mrb_define_method(mrb, sym, "to_s",    sym_to_s,    MRB_ARGS_NONE());          /* 15.2.11.3.3 */
  mrb_define_method(mrb, sym, "to_sym",  sym_to_sym,  MRB_ARGS_NONE());          /* 15.2.11.3.4 */
  mrb_define_method(mrb, sym, "inspect", sym_inspect, MRB_ARGS_NONE());          /* 15.2.11.3.5(x) */
  mrb_define_method(mrb, sym, "<=>",     sym_cmp,     MRB_ARGS_REQ(1));
}
/*
** variable.c - mruby variables
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#ifndef MRB_IV_SEGMENT_SIZE
#define MRB_IV_SEGMENT_SIZE 4
#endif

typedef struct segment {
  mrb_sym key[MRB_IV_SEGMENT_SIZE];
  mrb_value val[MRB_IV_SEGMENT_SIZE];
  struct segment *next;
} segment;

/* Instance variable table structure */
typedef struct iv_tbl {
  segment *rootseg;
  size_t size;
  size_t last_len;
} iv_tbl;

/* Creates the instance variable table. */
static iv_tbl*
iv_new(mrb_state *mrb)
{
  iv_tbl *t;

  t = (iv_tbl*)mrb_malloc(mrb, sizeof(iv_tbl));
  t->size = 0;
  t->rootseg =  NULL;
  t->last_len = 0;

  return t;
}

/* Set the value for the symbol in the instance variable table. */
static void
iv_put(mrb_state *mrb, iv_tbl *t, mrb_sym sym, mrb_value val)
{
  segment *seg;
  segment *prev = NULL;
  segment *matched_seg = NULL;
  size_t matched_idx = 0;
  size_t i;

  if (t == NULL) return;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_IV_SEGMENT_SIZE; i++) {
      mrb_sym key = seg->key[i];
      /* Found room in last segment after last_len */
      if (!seg->next && i >= t->last_len) {
        seg->key[i] = sym;
        seg->val[i] = val;
        t->last_len = i+1;
        t->size++;
        return;
      }
      if (!matched_seg && key == 0) {
        matched_seg = seg;
        matched_idx = i;
      }
      else if (key == sym) {
        seg->val[i] = val;
        return;
      }
    }
    prev = seg;
    seg = seg->next;
  }

  /* Not found */
  if (matched_seg) {
    matched_seg->key[matched_idx] = sym;
    matched_seg->val[matched_idx] = val;
    t->size++;
    return;
  }

  seg = (segment*)mrb_malloc(mrb, sizeof(segment));
  seg->next = NULL;
  seg->key[0] = sym;
  seg->val[0] = val;
  t->last_len = 1;
  t->size++;
  if (prev) {
    prev->next = seg;
  }
  else {
    t->rootseg = seg;
  }
}

/* Get a value for a symbol from the instance variable table. */
static mrb_bool
iv_get(mrb_state *mrb, iv_tbl *t, mrb_sym sym, mrb_value *vp)
{
  segment *seg;
  size_t i;

  if (t == NULL) return FALSE;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_IV_SEGMENT_SIZE; i++) {
      mrb_sym key = seg->key[i];

      if (!seg->next && i >= t->last_len) {
        return FALSE;
      }
      if (key == sym) {
        if (vp) *vp = seg->val[i];
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Deletes the value for the symbol from the instance variable table. */
static mrb_bool
iv_del(mrb_state *mrb, iv_tbl *t, mrb_sym sym, mrb_value *vp)
{
  segment *seg;
  size_t i;

  if (t == NULL) return FALSE;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_IV_SEGMENT_SIZE; i++) {
      mrb_sym key = seg->key[i];

      if (!seg->next && i >= t->last_len) {
        return FALSE;
      }
      if (key == sym) {
        t->size--;
        seg->key[i] = 0;
        if (vp) *vp = seg->val[i];
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Iterates over the instance variable table. */
static void
iv_foreach(mrb_state *mrb, iv_tbl *t, mrb_iv_foreach_func *func, void *p)
{
  segment *seg;
  size_t i;

  if (t == NULL) return;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_IV_SEGMENT_SIZE; i++) {
      mrb_sym key = seg->key[i];

      /* no value in last segment after last_len */
      if (!seg->next && i >= t->last_len) {
        return;
      }
      if (key != 0) {
        if ((*func)(mrb, key, seg->val[i], p) != 0) {
          return;
        }
      }
    }
    seg = seg->next;
  }
  return;
}

/* Get the size of the instance variable table. */
static size_t
iv_size(mrb_state *mrb, iv_tbl *t)
{
  segment *seg;
  size_t size = 0;

  if (t == NULL) return 0;
  if (t->size > 0) return t->size;
  seg = t->rootseg;
  while (seg) {
    if (seg->next == NULL) {
      size += t->last_len;
      return size;
    }
    seg = seg->next;
    size += MRB_IV_SEGMENT_SIZE;
  }
  /* empty iv_tbl */
  return 0;
}

/* Copy the instance variable table. */
static iv_tbl*
iv_copy(mrb_state *mrb, iv_tbl *t)
{
  segment *seg;
  iv_tbl *t2;

  size_t i;

  seg = t->rootseg;
  t2 = iv_new(mrb);

  while (seg != NULL) {
    for (i=0; i<MRB_IV_SEGMENT_SIZE; i++) {
      mrb_sym key = seg->key[i];
      mrb_value val = seg->val[i];

      if ((seg->next == NULL) && (i >= t->last_len)) {
        return t2;
      }
      iv_put(mrb, t2, key, val);
    }
    seg = seg->next;
  }
  return t2;
}

/* Free memory of the instance variable table. */
static void
iv_free(mrb_state *mrb, iv_tbl *t)
{
  segment *seg;

  seg = t->rootseg;
  while (seg) {
    segment *p = seg;
    seg = seg->next;
    mrb_free(mrb, p);
  }
  mrb_free(mrb, t);
}

static int
iv_mark_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_gc_mark_value(mrb, v);
  return 0;
}

static void
mark_tbl(mrb_state *mrb, iv_tbl *t)
{
  iv_foreach(mrb, t, iv_mark_i, 0);
}

void
mrb_gc_mark_gv(mrb_state *mrb)
{
  mark_tbl(mrb, mrb->globals);
}

void
mrb_gc_free_gv(mrb_state *mrb)
{
  if (mrb->globals)
    iv_free(mrb, mrb->globals);
}

void
mrb_gc_mark_iv(mrb_state *mrb, struct RObject *obj)
{
  mark_tbl(mrb, obj->iv);
}

size_t
mrb_gc_mark_iv_size(mrb_state *mrb, struct RObject *obj)
{
  return iv_size(mrb, obj->iv);
}

void
mrb_gc_free_iv(mrb_state *mrb, struct RObject *obj)
{
  if (obj->iv) {
    iv_free(mrb, obj->iv);
  }
}

mrb_value
mrb_vm_special_get(mrb_state *mrb, mrb_sym i)
{
  return mrb_fixnum_value(0);
}

void
mrb_vm_special_set(mrb_state *mrb, mrb_sym i, mrb_value v)
{
}

static mrb_bool
obj_iv_p(mrb_value obj)
{
  switch (mrb_type(obj)) {
    case MRB_TT_OBJECT:
    case MRB_TT_CLASS:
    case MRB_TT_MODULE:
    case MRB_TT_SCLASS:
    case MRB_TT_HASH:
    case MRB_TT_DATA:
    case MRB_TT_EXCEPTION:
      return TRUE;
    default:
      return FALSE;
  }
}

MRB_API mrb_value
mrb_obj_iv_get(mrb_state *mrb, struct RObject *obj, mrb_sym sym)
{
  mrb_value v;

  if (obj->iv && iv_get(mrb, obj->iv, sym, &v))
    return v;
  return mrb_nil_value();
}

MRB_API mrb_value
mrb_iv_get(mrb_state *mrb, mrb_value obj, mrb_sym sym)
{
  if (obj_iv_p(obj)) {
    return mrb_obj_iv_get(mrb, mrb_obj_ptr(obj), sym);
  }
  return mrb_nil_value();
}

static inline void assign_class_name(mrb_state *mrb, struct RObject *obj, mrb_sym sym, mrb_value v);

void
mrb_obj_iv_set_force(mrb_state *mrb, struct RObject *obj, mrb_sym sym, mrb_value v)
{
  assign_class_name(mrb, obj, sym, v);
  if (!obj->iv) {
    obj->iv = iv_new(mrb);
  }
  iv_put(mrb, obj->iv, sym, v);
  mrb_write_barrier(mrb, (struct RBasic*)obj);
}

MRB_API void
mrb_obj_iv_set(mrb_state *mrb, struct RObject *obj, mrb_sym sym, mrb_value v)
{
  mrb_check_frozen(mrb, obj);
  mrb_obj_iv_set_force(mrb, obj, sym, v);
}

/* Iterates over the instance variable table. */
MRB_API void
mrb_iv_foreach(mrb_state *mrb, mrb_value obj, mrb_iv_foreach_func *func, void *p)
{
  if (!obj_iv_p(obj)) return;
  iv_foreach(mrb, mrb_obj_ptr(obj)->iv, func, p);
}

static inline mrb_bool
namespace_p(enum mrb_vtype tt)
{
  return tt == MRB_TT_CLASS || tt == MRB_TT_MODULE ? TRUE : FALSE;
}

static inline void
assign_class_name(mrb_state *mrb, struct RObject *obj, mrb_sym sym, mrb_value v)
{
  if (namespace_p(obj->tt) && namespace_p(mrb_type(v))) {
    struct RObject *c = mrb_obj_ptr(v);
    if (obj != c && ISUPPER(mrb_sym_name_len(mrb, sym, NULL)[0])) {
      mrb_sym id_classname = mrb_intern_lit(mrb, "__classname__");
      mrb_value o = mrb_obj_iv_get(mrb, c, id_classname);

      if (mrb_nil_p(o)) {
        mrb_sym id_outer = mrb_intern_lit(mrb, "__outer__");
        o = mrb_obj_iv_get(mrb, c, id_outer);

        if (mrb_nil_p(o)) {
          if ((struct RClass *)obj == mrb->object_class) {
            mrb_obj_iv_set_force(mrb, c, id_classname, mrb_symbol_value(sym));
          }
          else {
            mrb_obj_iv_set_force(mrb, c, id_outer, mrb_obj_value(obj));
          }
        }
      }
    }
  }
}

MRB_API void
mrb_iv_set(mrb_state *mrb, mrb_value obj, mrb_sym sym, mrb_value v)
{
  if (obj_iv_p(obj)) {
    mrb_obj_iv_set(mrb, mrb_obj_ptr(obj), sym, v);
  }
  else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "cannot set instance variable");
  }
}

MRB_API mrb_bool
mrb_obj_iv_defined(mrb_state *mrb, struct RObject *obj, mrb_sym sym)
{
  iv_tbl *t;

  t = obj->iv;
  if (t) {
    return iv_get(mrb, t, sym, NULL);
  }
  return FALSE;
}

MRB_API mrb_bool
mrb_iv_defined(mrb_state *mrb, mrb_value obj, mrb_sym sym)
{
  if (!obj_iv_p(obj)) return FALSE;
  return mrb_obj_iv_defined(mrb, mrb_obj_ptr(obj), sym);
}

MRB_API mrb_bool
mrb_iv_name_sym_p(mrb_state *mrb, mrb_sym iv_name)
{
  const char *s;
  mrb_int len;

  s = mrb_sym_name_len(mrb, iv_name, &len);
  if (len < 2) return FALSE;
  if (s[0] != '@') return FALSE;
  if (ISDIGIT(s[1])) return FALSE;
  return mrb_ident_p(s+1, len-1);
}

MRB_API void
mrb_iv_name_sym_check(mrb_state *mrb, mrb_sym iv_name)
{
  if (!mrb_iv_name_sym_p(mrb, iv_name)) {
    mrb_name_error(mrb, iv_name, "'%n' is not allowed as an instance variable name", iv_name);
  }
}

MRB_API void
mrb_iv_copy(mrb_state *mrb, mrb_value dest, mrb_value src)
{
  struct RObject *d = mrb_obj_ptr(dest);
  struct RObject *s = mrb_obj_ptr(src);

  if (d->iv) {
    iv_free(mrb, d->iv);
    d->iv = 0;
  }
  if (s->iv) {
    mrb_write_barrier(mrb, (struct RBasic*)d);
    d->iv = iv_copy(mrb, s->iv);
  }
}

static int
inspect_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_value str = *(mrb_value*)p;
  const char *s;
  mrb_int len;
  mrb_value ins;
  char *sp = RSTRING_PTR(str);

  /* need not to show internal data */
  if (sp[0] == '-') { /* first element */
    sp[0] = '#';
    mrb_str_cat_lit(mrb, str, " ");
  }
  else {
    mrb_str_cat_lit(mrb, str, ", ");
  }
  s = mrb_sym_name_len(mrb, sym, &len);
  mrb_str_cat(mrb, str, s, len);
  mrb_str_cat_lit(mrb, str, "=");
  if (mrb_object_p(v)) {
    ins = mrb_any_to_s(mrb, v);
  }
  else {
    ins = mrb_inspect(mrb, v);
  }
  mrb_str_cat_str(mrb, str, ins);
  return 0;
}

mrb_value
mrb_obj_iv_inspect(mrb_state *mrb, struct RObject *obj)
{
  iv_tbl *t = obj->iv;
  size_t len = iv_size(mrb, t);

  if (len > 0) {
    const char *cn = mrb_obj_classname(mrb, mrb_obj_value(obj));
    mrb_value str = mrb_str_new_capa(mrb, 30);

    mrb_str_cat_lit(mrb, str, "-<");
    mrb_str_cat_cstr(mrb, str, cn);
    mrb_str_cat_lit(mrb, str, ":");
    mrb_str_cat_str(mrb, str, mrb_ptr_to_str(mrb, obj));

    iv_foreach(mrb, t, inspect_i, &str);
    mrb_str_cat_lit(mrb, str, ">");
    return str;
  }
  return mrb_any_to_s(mrb, mrb_obj_value(obj));
}

MRB_API mrb_value
mrb_iv_remove(mrb_state *mrb, mrb_value obj, mrb_sym sym)
{
  if (obj_iv_p(obj)) {
    iv_tbl *t = mrb_obj_ptr(obj)->iv;
    mrb_value val;

    mrb_check_frozen(mrb, mrb_obj_ptr(obj));
    if (iv_del(mrb, t, sym, &val)) {
      return val;
    }
  }
  return mrb_undef_value();
}

static int
iv_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_value ary;
  const char* s;
  mrb_int len;

  ary = *(mrb_value*)p;
  s = mrb_sym_name_len(mrb, sym, &len);
  if (len > 1 && s[0] == '@' && s[1] != '@') {
    mrb_ary_push(mrb, ary, mrb_symbol_value(sym));
  }
  return 0;
}

/* 15.3.1.3.23 */
/*
 *  call-seq:
 *     obj.instance_variables    -> array
 *
 *  Returns an array of instance variable names for the receiver. Note
 *  that simply defining an accessor does not create the corresponding
 *  instance variable.
 *
 *     class Fred
 *       attr_accessor :a1
 *       def initialize
 *         @iv = 3
 *       end
 *     end
 *     Fred.new.instance_variables   #=> [:@iv]
 */
mrb_value
mrb_obj_instance_variables(mrb_state *mrb, mrb_value self)
{
  mrb_value ary;

  ary = mrb_ary_new(mrb);
  if (obj_iv_p(self)) {
    iv_foreach(mrb, mrb_obj_ptr(self)->iv, iv_i, &ary);
  }
  return ary;
}

static int
cv_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_value ary;
  const char* s;
  mrb_int len;

  ary = *(mrb_value*)p;
  s = mrb_sym_name_len(mrb, sym, &len);
  if (len > 2 && s[0] == '@' && s[1] == '@') {
    mrb_ary_push(mrb, ary, mrb_symbol_value(sym));
  }
  return 0;
}

/* 15.2.2.4.19 */
/*
 *  call-seq:
 *     mod.class_variables(inherit=true)   -> array
 *
 *  Returns an array of the names of class variables in <i>mod</i>.
 *
 *     class One
 *       @@var1 = 1
 *     end
 *     class Two < One
 *       @@var2 = 2
 *     end
 *     One.class_variables   #=> [:@@var1]
 *     Two.class_variables   #=> [:@@var2]
 */
mrb_value
mrb_mod_class_variables(mrb_state *mrb, mrb_value mod)
{
  mrb_value ary;
  struct RClass *c;
  mrb_bool inherit = TRUE;

  mrb_get_args(mrb, "|b", &inherit);
  ary = mrb_ary_new(mrb);
  c = mrb_class_ptr(mod);
  while (c) {
    iv_foreach(mrb, c->iv, cv_i, &ary);
    if (!inherit) break;
    c = c->super;
  }
  return ary;
}

mrb_value
mrb_mod_cv_get(mrb_state *mrb, struct RClass *c, mrb_sym sym)
{
  struct RClass * cls = c;
  mrb_value v;
  int given = FALSE;

  while (c) {
    if (c->iv && iv_get(mrb, c->iv, sym, &v)) {
      given = TRUE;
    }
    c = c->super;
  }
  if (given) return v;
  if (cls && cls->tt == MRB_TT_SCLASS) {
    mrb_value klass;

    klass = mrb_obj_iv_get(mrb, (struct RObject *)cls,
                           mrb_intern_lit(mrb, "__attached__"));
    c = mrb_class_ptr(klass);
    if (c->tt == MRB_TT_CLASS || c->tt == MRB_TT_MODULE) {
      given = FALSE;
      while (c) {
        if (c->iv && iv_get(mrb, c->iv, sym, &v)) {
          given = TRUE;
        }
        c = c->super;
      }
      if (given) return v;
    }
  }
  mrb_name_error(mrb, sym, "uninitialized class variable %n in %C", sym, cls);
  /* not reached */
  return mrb_nil_value();
}

MRB_API mrb_value
mrb_cv_get(mrb_state *mrb, mrb_value mod, mrb_sym sym)
{
  return mrb_mod_cv_get(mrb, mrb_class_ptr(mod), sym);
}

MRB_API void
mrb_mod_cv_set(mrb_state *mrb, struct RClass *c, mrb_sym sym, mrb_value v)
{
  struct RClass * cls = c;

  while (c) {
    iv_tbl *t = c->iv;

    if (iv_get(mrb, t, sym, NULL)) {
      mrb_check_frozen(mrb, c);
      iv_put(mrb, t, sym, v);
      mrb_write_barrier(mrb, (struct RBasic*)c);
      return;
    }
    c = c->super;
  }

  if (cls && cls->tt == MRB_TT_SCLASS) {
    mrb_value klass;

    klass = mrb_obj_iv_get(mrb, (struct RObject*)cls,
                           mrb_intern_lit(mrb, "__attached__"));
    switch (mrb_type(klass)) {
    case MRB_TT_CLASS:
    case MRB_TT_MODULE:
    case MRB_TT_SCLASS:
      c = mrb_class_ptr(klass);
      break;
    default:
      c = cls;
      break;
    }
  }
  else{
    c = cls;
  }

  mrb_check_frozen(mrb, c);
  if (!c->iv) {
    c->iv = iv_new(mrb);
  }

  iv_put(mrb, c->iv, sym, v);
  mrb_write_barrier(mrb, (struct RBasic*)c);
}

MRB_API void
mrb_cv_set(mrb_state *mrb, mrb_value mod, mrb_sym sym, mrb_value v)
{
  mrb_mod_cv_set(mrb, mrb_class_ptr(mod), sym, v);
}

mrb_bool
mrb_mod_cv_defined(mrb_state *mrb, struct RClass * c, mrb_sym sym)
{
  while (c) {
    iv_tbl *t = c->iv;
    if (iv_get(mrb, t, sym, NULL)) return TRUE;
    c = c->super;
  }

  return FALSE;
}

MRB_API mrb_bool
mrb_cv_defined(mrb_state *mrb, mrb_value mod, mrb_sym sym)
{
  return mrb_mod_cv_defined(mrb, mrb_class_ptr(mod), sym);
}

mrb_value
mrb_vm_cv_get(mrb_state *mrb, mrb_sym sym)
{
  struct RClass *c;

  struct RProc *p = mrb->c->ci->proc;

  for (;;) {
    c = MRB_PROC_TARGET_CLASS(p);
    if (c->tt != MRB_TT_SCLASS) break;
    p = p->upper;
  }
  return mrb_mod_cv_get(mrb, c, sym);
}

void
mrb_vm_cv_set(mrb_state *mrb, mrb_sym sym, mrb_value v)
{
  struct RClass *c;
  struct RProc *p = mrb->c->ci->proc;

  for (;;) {
    c = MRB_PROC_TARGET_CLASS(p);
    if (c->tt != MRB_TT_SCLASS) break;
    p = p->upper;
  }
  mrb_mod_cv_set(mrb, c, sym, v);
}

static void
mod_const_check(mrb_state *mrb, mrb_value mod)
{
  switch (mrb_type(mod)) {
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    break;
  default:
    mrb_raise(mrb, E_TYPE_ERROR, "constant look-up for non class/module");
    break;
  }
}

static mrb_value
const_get(mrb_state *mrb, struct RClass *base, mrb_sym sym)
{
  struct RClass *c = base;
  mrb_value v;
  mrb_bool retry = FALSE;
  mrb_value name;

L_RETRY:
  while (c) {
    if (c->iv) {
      if (iv_get(mrb, c->iv, sym, &v))
        return v;
    }
    c = c->super;
  }
  if (!retry && base->tt == MRB_TT_MODULE) {
    c = mrb->object_class;
    retry = TRUE;
    goto L_RETRY;
  }
  name = mrb_symbol_value(sym);
  return mrb_funcall_argv(mrb, mrb_obj_value(base), mrb_intern_lit(mrb, "const_missing"), 1, &name);
}

MRB_API mrb_value
mrb_const_get(mrb_state *mrb, mrb_value mod, mrb_sym sym)
{
  mod_const_check(mrb, mod);
  return const_get(mrb, mrb_class_ptr(mod), sym);
}

mrb_value
mrb_vm_const_get(mrb_state *mrb, mrb_sym sym)
{
  struct RClass *c;
  struct RClass *c2;
  mrb_value v;
  struct RProc *proc;

  c = MRB_PROC_TARGET_CLASS(mrb->c->ci->proc);
  if (iv_get(mrb, c->iv, sym, &v)) {
    return v;
  }
  c2 = c;
  while (c2 && c2->tt == MRB_TT_SCLASS) {
    mrb_value klass;

    if (!iv_get(mrb, c2->iv, mrb_intern_lit(mrb, "__attached__"), &klass)) {
      c2 = NULL;
      break;
    }
    c2 = mrb_class_ptr(klass);
  }
  if (c2 && (c2->tt == MRB_TT_CLASS || c2->tt == MRB_TT_MODULE)) c = c2;
  mrb_assert(!MRB_PROC_CFUNC_P(mrb->c->ci->proc));
  proc = mrb->c->ci->proc;
  while (proc) {
    c2 = MRB_PROC_TARGET_CLASS(proc);
    if (c2 && iv_get(mrb, c2->iv, sym, &v)) {
      return v;
    }
    proc = proc->upper;
  }
  return const_get(mrb, c, sym);
}

MRB_API void
mrb_const_set(mrb_state *mrb, mrb_value mod, mrb_sym sym, mrb_value v)
{
  mod_const_check(mrb, mod);
  if (mrb_type(v) == MRB_TT_CLASS || mrb_type(v) == MRB_TT_MODULE) {
    mrb_class_name_class(mrb, mrb_class_ptr(mod), mrb_class_ptr(v), sym);
  }
  mrb_iv_set(mrb, mod, sym, v);
}

void
mrb_vm_const_set(mrb_state *mrb, mrb_sym sym, mrb_value v)
{
  struct RClass *c;

  c = MRB_PROC_TARGET_CLASS(mrb->c->ci->proc);
  mrb_obj_iv_set(mrb, (struct RObject*)c, sym, v);
}

MRB_API void
mrb_const_remove(mrb_state *mrb, mrb_value mod, mrb_sym sym)
{
  mod_const_check(mrb, mod);
  mrb_iv_remove(mrb, mod, sym);
}

MRB_API void
mrb_define_const(mrb_state *mrb, struct RClass *mod, const char *name, mrb_value v)
{
  mrb_obj_iv_set(mrb, (struct RObject*)mod, mrb_intern_cstr(mrb, name), v);
}

MRB_API void
mrb_define_global_const(mrb_state *mrb, const char *name, mrb_value val)
{
  mrb_define_const(mrb, mrb->object_class, name, val);
}

static int
const_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_value ary;
  const char* s;
  mrb_int len;

  ary = *(mrb_value*)p;
  s = mrb_sym_name_len(mrb, sym, &len);
  if (len >= 1 && ISUPPER(s[0])) {
    mrb_int i, alen = RARRAY_LEN(ary);

    for (i=0; i<alen; i++) {
      if (mrb_symbol(RARRAY_PTR(ary)[i]) == sym)
        break;
    }
    if (i==alen) {
      mrb_ary_push(mrb, ary, mrb_symbol_value(sym));
    }
  }
  return 0;
}

/* 15.2.2.4.24 */
/*
 *  call-seq:
 *     mod.constants    -> array
 *
 *  Returns an array of all names of contants defined in the receiver.
 */
mrb_value
mrb_mod_constants(mrb_state *mrb, mrb_value mod)
{
  mrb_value ary;
  mrb_bool inherit = TRUE;
  struct RClass *c = mrb_class_ptr(mod);

  mrb_get_args(mrb, "|b", &inherit);
  ary = mrb_ary_new(mrb);
  while (c) {
    iv_foreach(mrb, c->iv, const_i, &ary);
    if (!inherit) break;
    c = c->super;
    if (c == mrb->object_class) break;
  }
  return ary;
}

MRB_API mrb_value
mrb_gv_get(mrb_state *mrb, mrb_sym sym)
{
  mrb_value v;

  if (iv_get(mrb, mrb->globals, sym, &v))
    return v;
  return mrb_nil_value();
}

MRB_API void
mrb_gv_set(mrb_state *mrb, mrb_sym sym, mrb_value v)
{
  iv_tbl *t;

  if (!mrb->globals) {
    mrb->globals = iv_new(mrb);
  }
  t = mrb->globals;
  iv_put(mrb, t, sym, v);
}

MRB_API void
mrb_gv_remove(mrb_state *mrb, mrb_sym sym)
{
  iv_del(mrb, mrb->globals, sym, NULL);
}

static int
gv_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  mrb_value ary;

  ary = *(mrb_value*)p;
  mrb_ary_push(mrb, ary, mrb_symbol_value(sym));
  return 0;
}

/* 15.3.1.2.4  */
/* 15.3.1.3.14 */
/*
 *  call-seq:
 *     global_variables    -> array
 *
 *  Returns an array of the names of global variables.
 *
 *     global_variables.grep /std/   #=> [:$stdin, :$stdout, :$stderr]
 */
mrb_value
mrb_f_global_variables(mrb_state *mrb, mrb_value self)
{
  iv_tbl *t = mrb->globals;
  mrb_value ary = mrb_ary_new(mrb);

  iv_foreach(mrb, t, gv_i, &ary);
  return ary;
}

static mrb_bool
mrb_const_defined_0(mrb_state *mrb, mrb_value mod, mrb_sym id, mrb_bool exclude, mrb_bool recurse)
{
  struct RClass *klass = mrb_class_ptr(mod);
  struct RClass *tmp;
  mrb_bool mod_retry = FALSE;

  tmp = klass;
retry:
  while (tmp) {
    if (iv_get(mrb, tmp->iv, id, NULL)) {
      return TRUE;
    }
    if (!recurse && (klass != mrb->object_class)) break;
    tmp = tmp->super;
  }
  if (!exclude && !mod_retry && (klass->tt == MRB_TT_MODULE)) {
    mod_retry = TRUE;
    tmp = mrb->object_class;
    goto retry;
  }
  return FALSE;
}

MRB_API mrb_bool
mrb_const_defined(mrb_state *mrb, mrb_value mod, mrb_sym id)
{
  return mrb_const_defined_0(mrb, mod, id, TRUE, TRUE);
}

MRB_API mrb_bool
mrb_const_defined_at(mrb_state *mrb, mrb_value mod, mrb_sym id)
{
  return mrb_const_defined_0(mrb, mod, id, TRUE, FALSE);
}

MRB_API mrb_value
mrb_attr_get(mrb_state *mrb, mrb_value obj, mrb_sym id)
{
  return mrb_iv_get(mrb, obj, id);
}

struct csym_arg {
  struct RClass *c;
  mrb_sym sym;
};

static int
csym_i(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  struct csym_arg *a = (struct csym_arg*)p;
  struct RClass *c = a->c;

  if (mrb_type(v) == c->tt && mrb_class_ptr(v) == c) {
    a->sym = sym;
    return 1;     /* stop iteration */
  }
  return 0;
}

static mrb_sym
find_class_sym(mrb_state *mrb, struct RClass *outer, struct RClass *c)
{
  struct csym_arg arg;

  if (!outer) return 0;
  if (outer == c) return 0;
  arg.c = c;
  arg.sym = 0;
  iv_foreach(mrb, outer->iv, csym_i, &arg);
  return arg.sym;
}

static struct RClass*
outer_class(mrb_state *mrb, struct RClass *c)
{
  mrb_value ov;

  ov = mrb_obj_iv_get(mrb, (struct RObject*)c, mrb_intern_lit(mrb, "__outer__"));
  if (mrb_nil_p(ov)) return NULL;
  switch (mrb_type(ov)) {
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
    return mrb_class_ptr(ov);
  default:
    break;
  }
  return NULL;
}

static mrb_bool
detect_outer_loop(mrb_state *mrb, struct RClass *c)
{
  struct RClass *t = c;         /* tortoise */
  struct RClass *h = c;         /* hare */

  for (;;) {
    if (h == NULL) return FALSE;
    h = outer_class(mrb, h);
    if (h == NULL) return FALSE;
    h = outer_class(mrb, h);
    t = outer_class(mrb, t);
    if (t == h) return TRUE;
  }
}

mrb_value
mrb_class_find_path(mrb_state *mrb, struct RClass *c)
{
  struct RClass *outer;
  mrb_value path;
  mrb_sym name;
  const char *str;
  mrb_int len;

  if (detect_outer_loop(mrb, c)) return mrb_nil_value();
  outer = outer_class(mrb, c);
  if (outer == NULL) return mrb_nil_value();
  name = find_class_sym(mrb, outer, c);
  if (name == 0) return mrb_nil_value();
  str = mrb_class_name(mrb, outer);
  path = mrb_str_new_capa(mrb, 40);
  mrb_str_cat_cstr(mrb, path, str);
  mrb_str_cat_cstr(mrb, path, "::");

  str = mrb_sym_name_len(mrb, name, &len);
  mrb_str_cat(mrb, path, str, len);
  if (RSTRING_PTR(path)[0] != '#') {
    iv_del(mrb, c->iv, mrb_intern_lit(mrb, "__outer__"), NULL);
    iv_put(mrb, c->iv, mrb_intern_lit(mrb, "__classname__"), path);
    mrb_field_write_barrier_value(mrb, (struct RBasic*)c, path);
    path = mrb_str_dup(mrb, path);
  }
  return path;
}

#define identchar(c) (ISALNUM(c) || (c) == '_' || !ISASCII(c))

mrb_bool
mrb_ident_p(const char *s, mrb_int len)
{
  mrb_int i;

  for (i = 0; i < len; i++) {
    if (!identchar(s[i])) return FALSE;
  }
  return TRUE;
}
#include <mruby.h>
#include <mruby/variable.h>

void
mrb_init_version(mrb_state* mrb)
{
  mrb_value mruby_version = mrb_str_new_lit(mrb, MRUBY_VERSION);

  mrb_define_global_const(mrb, "RUBY_VERSION", mrb_str_new_lit(mrb, MRUBY_RUBY_VERSION));
  mrb_define_global_const(mrb, "RUBY_ENGINE", mrb_str_new_lit(mrb, MRUBY_RUBY_ENGINE));
  mrb_define_global_const(mrb, "RUBY_ENGINE_VERSION", mruby_version);
  mrb_define_global_const(mrb, "MRUBY_VERSION", mruby_version);
  mrb_define_global_const(mrb, "MRUBY_RELEASE_NO", mrb_fixnum_value(MRUBY_RELEASE_NO));
  mrb_define_global_const(mrb, "MRUBY_RELEASE_DATE", mrb_str_new_lit(mrb, MRUBY_RELEASE_DATE));
  mrb_define_global_const(mrb, "MRUBY_DESCRIPTION", mrb_str_new_lit(mrb, MRUBY_DESCRIPTION));
  mrb_define_global_const(mrb, "MRUBY_COPYRIGHT", mrb_str_new_lit(mrb, MRUBY_COPYRIGHT));
}
/*
** vm.c - virtual machine for mruby
**
** See Copyright Notice in mruby.h
*/

#include <stddef.h>
#include <stdarg.h>
#ifndef MRB_WITHOUT_FLOAT
#include <math.h>
#endif
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/irep.h>
#include <mruby/numeric.h>
#include <mruby/proc.h>
#include <mruby/range.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/opcode.h>
#include "value_array.h"
#include <mruby/throw.h>

#ifdef MRB_DISABLE_STDIO
#if defined(__cplusplus)
extern "C" {
#endif
void abort(void);
#if defined(__cplusplus)
}  /* extern "C" { */
#endif
#endif

#define STACK_INIT_SIZE 128
#define CALLINFO_INIT_SIZE 32

#ifndef ENSURE_STACK_INIT_SIZE
#define ENSURE_STACK_INIT_SIZE 16
#endif

#ifndef RESCUE_STACK_INIT_SIZE
#define RESCUE_STACK_INIT_SIZE 16
#endif

/* Define amount of linear stack growth. */
#ifndef MRB_STACK_GROWTH
#define MRB_STACK_GROWTH 128
#endif

/* Maximum mrb_funcall() depth. Should be set lower on memory constrained systems. */
#ifndef MRB_FUNCALL_DEPTH_MAX
#define MRB_FUNCALL_DEPTH_MAX 512
#endif

/* Maximum depth of ecall() recursion. */
#ifndef MRB_ECALL_DEPTH_MAX
#define MRB_ECALL_DEPTH_MAX 512
#endif

/* Maximum stack depth. Should be set lower on memory constrained systems.
The value below allows about 60000 recursive calls in the simplest case. */
#ifndef MRB_STACK_MAX
#define MRB_STACK_MAX (0x40000 - MRB_STACK_GROWTH)
#endif

#ifdef VM_DEBUG
# define DEBUG(x) (x)
#else
# define DEBUG(x)
#endif


#ifndef MRB_GC_FIXED_ARENA
static void
mrb_gc_arena_shrink(mrb_state *mrb, int idx)
{
  mrb_gc *gc = &mrb->gc;
  int capa = gc->arena_capa;

  if (idx < capa / 4) {
    capa >>= 2;
    if (capa < MRB_GC_ARENA_SIZE) {
      capa = MRB_GC_ARENA_SIZE;
    }
    if (capa != gc->arena_capa) {
      gc->arena = (struct RBasic**)mrb_realloc(mrb, gc->arena, sizeof(struct RBasic*)*capa);
      gc->arena_capa = capa;
    }
  }
}
#else
#define mrb_gc_arena_shrink(mrb,idx)
#endif

#define CALL_MAXARGS 127

void mrb_method_missing(mrb_state *mrb, mrb_sym name, mrb_value self, mrb_value args);

static inline void
stack_clear(mrb_value *from, size_t count)
{
#ifndef MRB_NAN_BOXING
  const mrb_value mrb_value_zero = { { 0 } };

  while (count-- > 0) {
    *from++ = mrb_value_zero;
  }
#else
  while (count-- > 0) {
    SET_NIL_VALUE(*from);
    from++;
  }
#endif
}

static inline void
stack_copy(mrb_value *dst, const mrb_value *src, size_t size)
{
  while (size-- > 0) {
    *dst++ = *src++;
  }
}

static void
stack_init(mrb_state *mrb)
{
  struct mrb_context *c = mrb->c;

  /* mrb_assert(mrb->stack == NULL); */
  c->stbase = (mrb_value *)mrb_calloc(mrb, STACK_INIT_SIZE, sizeof(mrb_value));
  c->stend = c->stbase + STACK_INIT_SIZE;
  c->stack = c->stbase;

  /* mrb_assert(ci == NULL); */
  c->cibase = (mrb_callinfo *)mrb_calloc(mrb, CALLINFO_INIT_SIZE, sizeof(mrb_callinfo));
  c->ciend = c->cibase + CALLINFO_INIT_SIZE;
  c->ci = c->cibase;
  c->ci->target_class = mrb->object_class;
  c->ci->stackent = c->stack;
}

static inline void
envadjust(mrb_state *mrb, mrb_value *oldbase, mrb_value *newbase, size_t oldsize)
{
  mrb_callinfo *ci = mrb->c->cibase;

  if (newbase == oldbase) return;
  while (ci <= mrb->c->ci) {
    struct REnv *e = ci->env;
    mrb_value *st;

    if (e && MRB_ENV_STACK_SHARED_P(e) &&
        (st = e->stack) && oldbase <= st && st < oldbase+oldsize) {
      ptrdiff_t off = e->stack - oldbase;

      e->stack = newbase + off;
    }

    if (ci->proc && MRB_PROC_ENV_P(ci->proc) && ci->env != MRB_PROC_ENV(ci->proc)) {
      e = MRB_PROC_ENV(ci->proc);

      if (e && MRB_ENV_STACK_SHARED_P(e) &&
          (st = e->stack) && oldbase <= st && st < oldbase+oldsize) {
        ptrdiff_t off = e->stack - oldbase;

        e->stack = newbase + off;
      }
    }

    ci->stackent = newbase + (ci->stackent - oldbase);
    ci++;
  }
}

/** def rec ; $deep =+ 1 ; if $deep > 1000 ; return 0 ; end ; rec ; end  */

static void
stack_extend_alloc(mrb_state *mrb, mrb_int room)
{
  mrb_value *oldbase = mrb->c->stbase;
  mrb_value *newstack;
  size_t oldsize = mrb->c->stend - mrb->c->stbase;
  size_t size = oldsize;
  size_t off = mrb->c->stack - mrb->c->stbase;

  if (off > size) size = off;
#ifdef MRB_STACK_EXTEND_DOUBLING
  if ((size_t)room <= size)
    size *= 2;
  else
    size += room;
#else
  /* Use linear stack growth.
     It is slightly slower than doubling the stack space,
     but it saves memory on small devices. */
  if (room <= MRB_STACK_GROWTH)
    size += MRB_STACK_GROWTH;
  else
    size += room;
#endif

  newstack = (mrb_value *)mrb_realloc(mrb, mrb->c->stbase, sizeof(mrb_value) * size);
  if (newstack == NULL) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->stack_err));
  }
  stack_clear(&(newstack[oldsize]), size - oldsize);
  envadjust(mrb, oldbase, newstack, oldsize);
  mrb->c->stbase = newstack;
  mrb->c->stack = mrb->c->stbase + off;
  mrb->c->stend = mrb->c->stbase + size;

  /* Raise an exception if the new stack size will be too large,
     to prevent infinite recursion. However, do this only after resizing the stack, so mrb_raise has stack space to work with. */
  if (size > MRB_STACK_MAX) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->stack_err));
  }
}

MRB_API void
mrb_stack_extend(mrb_state *mrb, mrb_int room)
{
  if (mrb->c->stack + room >= mrb->c->stend) {
    stack_extend_alloc(mrb, room);
  }
}

static inline struct REnv*
uvenv(mrb_state *mrb, int up)
{
  struct RProc *proc = mrb->c->ci->proc;
  struct REnv *e;

  while (up--) {
    proc = proc->upper;
    if (!proc) return NULL;
  }
  e = MRB_PROC_ENV(proc);
  if (e) return e;              /* proc has enclosed env */
  else {
    mrb_callinfo *ci = mrb->c->ci;
    mrb_callinfo *cb = mrb->c->cibase;

    while (cb <= ci) {
      if (ci->proc == proc) {
        return ci->env;
      }
      ci--;
    }
  }
  return NULL;
}

static inline struct RProc*
top_proc(mrb_state *mrb, struct RProc *proc)
{
  while (proc->upper) {
    if (MRB_PROC_SCOPE_P(proc) || MRB_PROC_STRICT_P(proc))
      return proc;
    proc = proc->upper;
  }
  return proc;
}

#define CI_ACC_SKIP    -1
#define CI_ACC_DIRECT  -2
#define CI_ACC_RESUMED -3

static inline mrb_callinfo*
cipush(mrb_state *mrb)
{
  struct mrb_context *c = mrb->c;
  static const mrb_callinfo ci_zero = { 0 };
  mrb_callinfo *ci = c->ci;

  int ridx = ci->ridx;

  if (ci + 1 == c->ciend) {
    ptrdiff_t size = ci - c->cibase;

    c->cibase = (mrb_callinfo *)mrb_realloc(mrb, c->cibase, sizeof(mrb_callinfo)*size*2);
    c->ci = c->cibase + size;
    c->ciend = c->cibase + size * 2;
  }
  ci = ++c->ci;
  *ci = ci_zero;
  ci->epos = mrb->c->eidx;
  ci->ridx = ridx;

  return ci;
}

void
mrb_env_unshare(mrb_state *mrb, struct REnv *e)
{
  if (e == NULL) return;
  else {
    size_t len = (size_t)MRB_ENV_STACK_LEN(e);
    mrb_value *p;

    if (!MRB_ENV_STACK_SHARED_P(e)) return;
    if (e->cxt != mrb->c) return;
    if (e == mrb->c->cibase->env) return; /* for mirb */
    p = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value)*len);
    if (len > 0) {
      stack_copy(p, e->stack, len);
    }
    e->stack = p;
    MRB_ENV_UNSHARE_STACK(e);
    mrb_write_barrier(mrb, (struct RBasic *)e);
  }
}

static inline void
cipop(mrb_state *mrb)
{
  struct mrb_context *c = mrb->c;
  struct REnv *env = c->ci->env;

  c->ci--;
  if (env) mrb_env_unshare(mrb, env);
}

void mrb_exc_set(mrb_state *mrb, mrb_value exc);
static mrb_value mrb_run(mrb_state *mrb, struct RProc* proc, mrb_value self);

static void
ecall(mrb_state *mrb)
{
  struct RProc *p;
  struct mrb_context *c = mrb->c;
  mrb_callinfo *ci = c->ci;
  struct RObject *exc;
  struct REnv *env;
  ptrdiff_t cioff;
  int ai = mrb_gc_arena_save(mrb);
  uint16_t i;
  int nregs;

  if (c->eidx == 0) return;
  i = --c->eidx;

  /* restrict total call depth of ecall() */
  if (++mrb->ecall_nest > MRB_ECALL_DEPTH_MAX) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->stack_err));
  }
  p = c->ensure[i];
  if (!p) return;
  mrb_assert(!MRB_PROC_CFUNC_P(p));
  c->ensure[i] = NULL;
  nregs = p->upper->body.irep->nregs;
  if (ci->proc && !MRB_PROC_CFUNC_P(ci->proc) &&
      ci->proc->body.irep->nregs > nregs) {
    nregs = ci->proc->body.irep->nregs;
  }
  cioff = ci - c->cibase;
  ci = cipush(mrb);
  ci->stackent = mrb->c->stack;
  ci->mid = ci[-1].mid;
  ci->acc = CI_ACC_SKIP;
  ci->argc = 0;
  ci->proc = p;
  ci->target_class = MRB_PROC_TARGET_CLASS(p);
  env = MRB_PROC_ENV(p);
  mrb_assert(env);
  c->stack += nregs;
  exc = mrb->exc; mrb->exc = 0;
  if (exc) {
    mrb_gc_protect(mrb, mrb_obj_value(exc));
  }
  if (mrb->c->fib) {
    mrb_gc_protect(mrb, mrb_obj_value(mrb->c->fib));
  }
  mrb_run(mrb, p, env->stack[0]);
  mrb->c = c;
  c->ci = c->cibase + cioff;
  if (!mrb->exc) mrb->exc = exc;
  mrb_gc_arena_restore(mrb, ai);
  mrb->ecall_nest--;
}

#ifndef MRB_FUNCALL_ARGC_MAX
#define MRB_FUNCALL_ARGC_MAX 16
#endif

MRB_API mrb_value
mrb_funcall(mrb_state *mrb, mrb_value self, const char *name, mrb_int argc, ...)
{
  mrb_value argv[MRB_FUNCALL_ARGC_MAX];
  va_list ap;
  mrb_int i;
  mrb_sym mid = mrb_intern_cstr(mrb, name);

  if (argc > MRB_FUNCALL_ARGC_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Too long arguments. (limit=" MRB_STRINGIZE(MRB_FUNCALL_ARGC_MAX) ")");
  }

  va_start(ap, argc);
  for (i = 0; i < argc; i++) {
    argv[i] = va_arg(ap, mrb_value);
  }
  va_end(ap);
  return mrb_funcall_argv(mrb, self, mid, argc, argv);
}

static int
ci_nregs(mrb_callinfo *ci)
{
  struct RProc *p;
  int n = 0;

  if (!ci) return 3;
  p = ci->proc;
  if (!p) {
    if (ci->argc < 0) return 3;
    return ci->argc+2;
  }
  if (!MRB_PROC_CFUNC_P(p) && p->body.irep) {
    n = p->body.irep->nregs;
  }
  if (ci->argc < 0) {
    if (n < 3) n = 3; /* self + args + blk */
  }
  if (ci->argc > n) {
    n = ci->argc + 2; /* self + blk */
  }
  return n;
}

MRB_API mrb_value
mrb_funcall_with_block(mrb_state *mrb, mrb_value self, mrb_sym mid, mrb_int argc, const mrb_value *argv, mrb_value blk)
{
  mrb_value val;
  int ai = mrb_gc_arena_save(mrb);

  if (!mrb->jmp) {
    struct mrb_jmpbuf c_jmp;
    ptrdiff_t nth_ci = mrb->c->ci - mrb->c->cibase;

    MRB_TRY(&c_jmp) {
      mrb->jmp = &c_jmp;
      /* recursive call */
      val = mrb_funcall_with_block(mrb, self, mid, argc, argv, blk);
      mrb->jmp = 0;
    }
    MRB_CATCH(&c_jmp) { /* error */
      while (nth_ci < (mrb->c->ci - mrb->c->cibase)) {
        mrb->c->stack = mrb->c->ci->stackent;
        cipop(mrb);
      }
      mrb->jmp = 0;
      val = mrb_obj_value(mrb->exc);
    }
    MRB_END_EXC(&c_jmp);
    mrb->jmp = 0;
  }
  else {
    mrb_method_t m;
    struct RClass *c;
    mrb_callinfo *ci;
    int n = ci_nregs(mrb->c->ci);
    ptrdiff_t voff = -1;

    if (!mrb->c->stack) {
      stack_init(mrb);
    }
    if (argc < 0) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative argc for funcall (%i)", argc);
    }
    c = mrb_class(mrb, self);
    m = mrb_method_search_vm(mrb, &c, mid);
    if (MRB_METHOD_UNDEF_P(m)) {
      mrb_sym missing = mrb_intern_lit(mrb, "method_missing");
      mrb_value args = mrb_ary_new_from_values(mrb, argc, argv);
      m = mrb_method_search_vm(mrb, &c, missing);
      if (MRB_METHOD_UNDEF_P(m)) {
        mrb_method_missing(mrb, mid, self, args);
      }
      mrb_ary_unshift(mrb, args, mrb_symbol_value(mid));
      mrb_stack_extend(mrb, n+2);
      mrb->c->stack[n+1] = args;
      argc = -1;
    }
    if (mrb->c->ci - mrb->c->cibase > MRB_FUNCALL_DEPTH_MAX) {
      mrb_exc_raise(mrb, mrb_obj_value(mrb->stack_err));
    }
    ci = cipush(mrb);
    ci->mid = mid;
    ci->stackent = mrb->c->stack;
    ci->argc = (int)argc;
    ci->target_class = c;
    mrb->c->stack = mrb->c->stack + n;
    if (argc < 0) argc = 1;
    if (mrb->c->stbase <= argv && argv < mrb->c->stend) {
      voff = argv - mrb->c->stbase;
    }
    if (argc >= CALL_MAXARGS) {
      mrb_value args = mrb_ary_new_from_values(mrb, argc, argv);

      mrb->c->stack[1] = args;
      ci->argc = -1;
      argc = 1;
    }
    mrb_stack_extend(mrb, argc + 2);
    if (MRB_METHOD_PROC_P(m)) {
      struct RProc *p = MRB_METHOD_PROC(m);

      ci->proc = p;
      if (!MRB_PROC_CFUNC_P(p)) {
        mrb_stack_extend(mrb, p->body.irep->nregs + argc);
      }
    }
    if (voff >= 0) {
      argv = mrb->c->stbase + voff;
    }
    mrb->c->stack[0] = self;
    if (ci->argc > 0) {
      stack_copy(mrb->c->stack+1, argv, argc);
    }
    mrb->c->stack[argc+1] = blk;

    if (MRB_METHOD_CFUNC_P(m)) {
      ci->acc = CI_ACC_DIRECT;
      val = MRB_METHOD_CFUNC(m)(mrb, self);
      mrb->c->stack = mrb->c->ci->stackent;
      cipop(mrb);
    }
    else {
      ci->acc = CI_ACC_SKIP;
      val = mrb_run(mrb, MRB_METHOD_PROC(m), self);
    }
  }
  mrb_gc_arena_restore(mrb, ai);
  mrb_gc_protect(mrb, val);
  return val;
}

MRB_API mrb_value
mrb_funcall_argv(mrb_state *mrb, mrb_value self, mrb_sym mid, mrb_int argc, const mrb_value *argv)
{
  return mrb_funcall_with_block(mrb, self, mid, argc, argv, mrb_nil_value());
}

mrb_value
mrb_exec_irep(mrb_state *mrb, mrb_value self, struct RProc *p)
{
  mrb_callinfo *ci = mrb->c->ci;
  int keep, nregs;

  mrb->c->stack[0] = self;
  ci->proc = p;
  if (MRB_PROC_CFUNC_P(p)) {
    return MRB_PROC_CFUNC(p)(mrb, self);
  }
  nregs = p->body.irep->nregs;
  if (ci->argc < 0) keep = 3;
  else keep = ci->argc + 2;
  if (nregs < keep) {
    mrb_stack_extend(mrb, keep);
  }
  else {
    mrb_stack_extend(mrb, nregs);
    stack_clear(mrb->c->stack+keep, nregs-keep);
  }

  ci = cipush(mrb);
  ci->target_class = 0;
  ci->pc = p->body.irep->iseq;
  ci->stackent = mrb->c->stack;
  ci->acc = 0;

  return self;
}

/* 15.3.1.3.4  */
/* 15.3.1.3.44 */
/*
 *  call-seq:
 *     obj.send(symbol [, args...])        -> obj
 *     obj.__send__(symbol [, args...])      -> obj
 *
 *  Invokes the method identified by _symbol_, passing it any
 *  arguments specified. You can use <code>__send__</code> if the name
 *  +send+ clashes with an existing method in _obj_.
 *
 *     class Klass
 *       def hello(*args)
 *         "Hello " + args.join(' ')
 *       end
 *     end
 *     k = Klass.new
 *     k.send :hello, "gentle", "readers"   #=> "Hello gentle readers"
 */
mrb_value
mrb_f_send(mrb_state *mrb, mrb_value self)
{
  mrb_sym name;
  mrb_value block, *argv, *regs;
  mrb_int argc, i, len;
  mrb_method_t m;
  struct RClass *c;
  mrb_callinfo *ci;

  mrb_get_args(mrb, "n*&", &name, &argv, &argc, &block);
  ci = mrb->c->ci;
  if (ci->acc < 0) {
  funcall:
    return mrb_funcall_with_block(mrb, self, name, argc, argv, block);
  }

  c = mrb_class(mrb, self);
  m = mrb_method_search_vm(mrb, &c, name);
  if (MRB_METHOD_UNDEF_P(m)) {            /* call method_mising */
    goto funcall;
  }

  ci->mid = name;
  ci->target_class = c;
  regs = mrb->c->stack+1;
  /* remove first symbol from arguments */
  if (ci->argc >= 0) {
    for (i=0,len=ci->argc; i<len; i++) {
      regs[i] = regs[i+1];
    }
    ci->argc--;
  }
  else {                     /* variable length arguments */
    regs[0] = mrb_ary_subseq(mrb, regs[0], 1, RARRAY_LEN(regs[0]) - 1);
  }

  if (MRB_METHOD_CFUNC_P(m)) {
    if (MRB_METHOD_PROC_P(m)) {
      ci->proc = MRB_METHOD_PROC(m);
    }
    return MRB_METHOD_CFUNC(m)(mrb, self);
  }
  return mrb_exec_irep(mrb, self, MRB_METHOD_PROC(m));
}

static mrb_value
eval_under(mrb_state *mrb, mrb_value self, mrb_value blk, struct RClass *c)
{
  struct RProc *p;
  mrb_callinfo *ci;
  int nregs;

  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  ci = mrb->c->ci;
  if (ci->acc == CI_ACC_DIRECT) {
    return mrb_yield_with_class(mrb, blk, 1, &self, self, c);
  }
  ci->target_class = c;
  p = mrb_proc_ptr(blk);
  ci->proc = p;
  ci->argc = 1;
  ci->mid = ci[-1].mid;
  if (MRB_PROC_CFUNC_P(p)) {
    mrb_stack_extend(mrb, 3);
    mrb->c->stack[0] = self;
    mrb->c->stack[1] = self;
    mrb->c->stack[2] = mrb_nil_value();
    return MRB_PROC_CFUNC(p)(mrb, self);
  }
  nregs = p->body.irep->nregs;
  if (nregs < 3) nregs = 3;
  mrb_stack_extend(mrb, nregs);
  mrb->c->stack[0] = self;
  mrb->c->stack[1] = self;
  stack_clear(mrb->c->stack+2, nregs-2);
  ci = cipush(mrb);
  ci->target_class = 0;
  ci->pc = p->body.irep->iseq;
  ci->stackent = mrb->c->stack;
  ci->acc = 0;

  return self;
}

/* 15.2.2.4.35 */
/*
 *  call-seq:
 *     mod.class_eval {| | block }  -> obj
 *     mod.module_eval {| | block } -> obj
 *
 *  Evaluates block in the context of _mod_. This can
 *  be used to add methods to a class. <code>module_eval</code> returns
 *  the result of evaluating its argument.
 */
mrb_value
mrb_mod_module_eval(mrb_state *mrb, mrb_value mod)
{
  mrb_value a, b;

  if (mrb_get_args(mrb, "|S&", &a, &b) == 1) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "module_eval/class_eval with string not implemented");
  }
  return eval_under(mrb, mod, b, mrb_class_ptr(mod));
}

/* 15.3.1.3.18 */
/*
 *  call-seq:
 *     obj.instance_eval {| | block }                       -> obj
 *
 *  Evaluates the given block,within  the context of the receiver (_obj_).
 *  In order to set the context, the variable +self+ is set to _obj_ while
 *  the code is executing, giving the code access to _obj_'s
 *  instance variables. In the version of <code>instance_eval</code>
 *  that takes a +String+, the optional second and third
 *  parameters supply a filename and starting line number that are used
 *  when reporting compilation errors.
 *
 *     class KlassWithSecret
 *       def initialize
 *         @secret = 99
 *       end
 *     end
 *     k = KlassWithSecret.new
 *     k.instance_eval { @secret }   #=> 99
 */
mrb_value
mrb_obj_instance_eval(mrb_state *mrb, mrb_value self)
{
  mrb_value a, b;

  if (mrb_get_args(mrb, "|S&", &a, &b) == 1) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "instance_eval with string not implemented");
  }
  return eval_under(mrb, self, b, mrb_singleton_class_ptr(mrb, self));
}

MRB_API mrb_value
mrb_yield_with_class(mrb_state *mrb, mrb_value b, mrb_int argc, const mrb_value *argv, mrb_value self, struct RClass *c)
{
  struct RProc *p;
  mrb_sym mid = mrb->c->ci->mid;
  mrb_callinfo *ci;
  mrb_value val;
  int n;

  if (mrb_nil_p(b)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  ci = mrb->c->ci;
  n = ci_nregs(ci);
  if (ci - mrb->c->cibase > MRB_FUNCALL_DEPTH_MAX) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->stack_err));
  }
  p = mrb_proc_ptr(b);
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackent = mrb->c->stack;
  ci->argc = (int)argc;
  ci->target_class = c;
  ci->acc = CI_ACC_SKIP;
  n = MRB_PROC_CFUNC_P(p) ? (int)(argc+2) : p->body.irep->nregs;
  mrb->c->stack = mrb->c->stack + n;
  mrb_stack_extend(mrb, n);

  mrb->c->stack[0] = self;
  if (argc > 0) {
    stack_copy(mrb->c->stack+1, argv, argc);
  }
  mrb->c->stack[argc+1] = mrb_nil_value();

  if (MRB_PROC_CFUNC_P(p)) {
    val = MRB_PROC_CFUNC(p)(mrb, self);
    mrb->c->stack = mrb->c->ci->stackent;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }
  return val;
}

MRB_API mrb_value
mrb_yield_argv(mrb_state *mrb, mrb_value b, mrb_int argc, const mrb_value *argv)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_with_class(mrb, b, argc, argv, MRB_PROC_ENV(p)->stack[0], MRB_PROC_TARGET_CLASS(p));
}

MRB_API mrb_value
mrb_yield(mrb_state *mrb, mrb_value b, mrb_value arg)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_with_class(mrb, b, 1, &arg, MRB_PROC_ENV(p)->stack[0], MRB_PROC_TARGET_CLASS(p));
}

mrb_value
mrb_yield_cont(mrb_state *mrb, mrb_value b, mrb_value self, mrb_int argc, const mrb_value *argv)
{
  struct RProc *p;
  mrb_callinfo *ci;

  if (mrb_nil_p(b)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (!mrb_proc_p(b)) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }

  p = mrb_proc_ptr(b);
  ci = mrb->c->ci;

  mrb_stack_extend(mrb, 3);
  mrb->c->stack[1] = mrb_ary_new_from_values(mrb, argc, argv);
  mrb->c->stack[2] = mrb_nil_value();
  ci->argc = -1;
  return mrb_exec_irep(mrb, self, p);
}

static struct RBreak*
break_new(mrb_state *mrb, struct RProc *p, mrb_value val)
{
  struct RBreak *brk;

  brk = (struct RBreak*)mrb_obj_alloc(mrb, MRB_TT_BREAK, NULL);
  mrb_break_proc_set(brk, p);
  mrb_break_value_set(brk, val);

  return brk;
}

typedef enum {
  LOCALJUMP_ERROR_RETURN = 0,
  LOCALJUMP_ERROR_BREAK = 1,
  LOCALJUMP_ERROR_YIELD = 2
} localjump_error_kind;

static void
localjump_error(mrb_state *mrb, localjump_error_kind kind)
{
  char kind_str[3][7] = { "return", "break", "yield" };
  char kind_str_len[] = { 6, 5, 5 };
  static const char lead[] = "unexpected ";
  mrb_value msg;
  mrb_value exc;

  msg = mrb_str_new_capa(mrb, sizeof(lead) + 7);
  mrb_str_cat(mrb, msg, lead, sizeof(lead) - 1);
  mrb_str_cat(mrb, msg, kind_str[kind], kind_str_len[kind]);
  exc = mrb_exc_new_str(mrb, E_LOCALJUMP_ERROR, msg);
  mrb_exc_set(mrb, exc);
}

static void
argnum_error(mrb_state *mrb, mrb_int num)
{
  mrb_value exc;
  mrb_value str;
  mrb_int argc = mrb->c->ci->argc;

  if (argc < 0) {
    mrb_value args = mrb->c->stack[1];
    if (mrb_array_p(args)) {
      argc = RARRAY_LEN(args);
    }
  }
  if (mrb->c->ci->mid) {
    str = mrb_format(mrb, "'%n': wrong number of arguments (%i for %i)",
                     mrb->c->ci->mid, argc, num);
  }
  else {
    str = mrb_format(mrb, "wrong number of arguments (%i for %i)", argc, num);
  }
  exc = mrb_exc_new_str(mrb, E_ARGUMENT_ERROR, str);
  mrb_exc_set(mrb, exc);
}

#define ERR_PC_SET(mrb) mrb->c->ci->err = pc0;
#define ERR_PC_CLR(mrb) mrb->c->ci->err = 0;
#ifdef MRB_ENABLE_DEBUG_HOOK
#define CODE_FETCH_HOOK(mrb, irep, pc, regs) if ((mrb)->code_fetch_hook) (mrb)->code_fetch_hook((mrb), (irep), (pc), (regs));
#else
#define CODE_FETCH_HOOK(mrb, irep, pc, regs)
#endif

#ifdef MRB_BYTECODE_DECODE_OPTION
#define BYTECODE_DECODER(x) ((mrb)->bytecode_decoder)?(mrb)->bytecode_decoder((mrb), (x)):(x)
#else
#define BYTECODE_DECODER(x) (x)
#endif

#ifndef MRB_DISABLE_DIRECT_THREADING
#if defined __GNUC__ || defined __clang__ || defined __INTEL_COMPILER
#define DIRECT_THREADED
#endif
#endif /* ifndef MRB_DISABLE_DIRECT_THREADING */

#ifndef DIRECT_THREADED

#define INIT_DISPATCH for (;;) { insn = BYTECODE_DECODER(*pc); CODE_FETCH_HOOK(mrb, irep, pc, regs); switch (insn) {
#define CASE(insn,ops) case insn: pc0=pc++; FETCH_ ## ops ();; L_ ## insn ## _BODY:
#define NEXT break
#define JUMP NEXT
#define END_DISPATCH }}

#else

#define INIT_DISPATCH JUMP; return mrb_nil_value();
#define CASE(insn,ops) L_ ## insn: pc0=pc++; FETCH_ ## ops (); L_ ## insn ## _BODY:
#define NEXT insn=BYTECODE_DECODER(*pc); CODE_FETCH_HOOK(mrb, irep, pc, regs); goto *optable[insn]
#define JUMP NEXT

#define END_DISPATCH

#endif

MRB_API mrb_value
mrb_vm_run(mrb_state *mrb, struct RProc *proc, mrb_value self, unsigned int stack_keep)
{
  mrb_irep *irep = proc->body.irep;
  mrb_value result;
  struct mrb_context *c = mrb->c;
  ptrdiff_t cioff = c->ci - c->cibase;
  unsigned int nregs = irep->nregs;

  if (!c->stack) {
    stack_init(mrb);
  }
  if (stack_keep > nregs)
    nregs = stack_keep;
  mrb_stack_extend(mrb, nregs);
  stack_clear(c->stack + stack_keep, nregs - stack_keep);
  c->stack[0] = self;
  result = mrb_vm_exec(mrb, proc, irep->iseq);
  if (mrb->c != c) {
    if (mrb->c->fib) {
      mrb_write_barrier(mrb, (struct RBasic*)mrb->c->fib);
    }
    mrb->c = c;
  }
  else if (c->ci - c->cibase > cioff) {
    c->ci = c->cibase + cioff;
  }
  return result;
}

static mrb_bool
check_target_class(mrb_state *mrb)
{
  if (!mrb->c->ci->target_class) {
    mrb_value exc = mrb_exc_new_str_lit(mrb, E_TYPE_ERROR, "no target class or module");
    mrb_exc_set(mrb, exc);
    return FALSE;
  }
  return TRUE;
}

void mrb_hash_check_kdict(mrb_state *mrb, mrb_value self);

MRB_API mrb_value
mrb_vm_exec(mrb_state *mrb, struct RProc *proc, const mrb_code *pc)
{
  /* mrb_assert(MRB_PROC_CFUNC_P(proc)) */
  const mrb_code *pc0 = pc;
  mrb_irep *irep = proc->body.irep;
  mrb_value *pool = irep->pool;
  mrb_sym *syms = irep->syms;
  mrb_code insn;
  int ai = mrb_gc_arena_save(mrb);
  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  uint32_t a;
  uint16_t b;
  uint8_t c;
  mrb_sym mid;

#ifdef DIRECT_THREADED
  static void *optable[] = {
#define OPCODE(x,_) &&L_OP_ ## x,
#include "mruby/ops.h"
#undef OPCODE
  };
#endif

  mrb_bool exc_catched = FALSE;
RETRY_TRY_BLOCK:

  MRB_TRY(&c_jmp) {

  if (exc_catched) {
    exc_catched = FALSE;
    mrb_gc_arena_restore(mrb, ai);
    if (mrb->exc && mrb->exc->tt == MRB_TT_BREAK)
      goto L_BREAK;
    goto L_RAISE;
  }
  mrb->jmp = &c_jmp;
  mrb->c->ci->proc = proc;

#define regs (mrb->c->stack)
  INIT_DISPATCH {
    CASE(OP_NOP, Z) {
      /* do nothing */
      NEXT;
    }

    CASE(OP_MOVE, BB) {
      regs[a] = regs[b];
      NEXT;
    }

    CASE(OP_LOADL, BB) {
#ifdef MRB_WORD_BOXING
      mrb_value val = pool[b];
#ifndef MRB_WITHOUT_FLOAT
      if (mrb_float_p(val)) {
        val = mrb_float_value(mrb, mrb_float(val));
      }
#endif
      regs[a] = val;
#else
      regs[a] = pool[b];
#endif
      NEXT;
    }

    CASE(OP_LOADI, BB) {
      SET_INT_VALUE(regs[a], b);
      NEXT;
    }

    CASE(OP_LOADINEG, BB) {
      SET_INT_VALUE(regs[a], -b);
      NEXT;
    }

    CASE(OP_LOADI__1,B) goto L_LOADI;
    CASE(OP_LOADI_0,B) goto L_LOADI;
    CASE(OP_LOADI_1,B) goto L_LOADI;
    CASE(OP_LOADI_2,B) goto L_LOADI;
    CASE(OP_LOADI_3,B) goto L_LOADI;
    CASE(OP_LOADI_4,B) goto L_LOADI;
    CASE(OP_LOADI_5,B) goto L_LOADI;
    CASE(OP_LOADI_6,B) goto L_LOADI;
    CASE(OP_LOADI_7, B) {
    L_LOADI:
      SET_INT_VALUE(regs[a], (mrb_int)insn - (mrb_int)OP_LOADI_0);
      NEXT;
    }

    CASE(OP_LOADI16, BS) {
      SET_INT_VALUE(regs[a], (mrb_int)(int16_t)b);
      NEXT;
    }

    CASE(OP_LOADSYM, BB) {
      SET_SYM_VALUE(regs[a], syms[b]);
      NEXT;
    }

    CASE(OP_LOADNIL, B) {
      SET_NIL_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_LOADSELF, B) {
      regs[a] = regs[0];
      NEXT;
    }

    CASE(OP_LOADT, B) {
      SET_TRUE_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_LOADF, B) {
      SET_FALSE_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_GETGV, BB) {
      mrb_value val = mrb_gv_get(mrb, syms[b]);
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETGV, BB) {
      mrb_gv_set(mrb, syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETSV, BB) {
      mrb_value val = mrb_vm_special_get(mrb, syms[b]);
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETSV, BB) {
      mrb_vm_special_set(mrb, syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETIV, BB) {
      regs[a] = mrb_iv_get(mrb, regs[0], syms[b]);
      NEXT;
    }

    CASE(OP_SETIV, BB) {
      mrb_iv_set(mrb, regs[0], syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETCV, BB) {
      mrb_value val;
      ERR_PC_SET(mrb);
      val = mrb_vm_cv_get(mrb, syms[b]);
      ERR_PC_CLR(mrb);
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETCV, BB) {
      mrb_vm_cv_set(mrb, syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETCONST, BB) {
      mrb_value val;
      mrb_sym sym = syms[b];

      ERR_PC_SET(mrb);
      val = mrb_vm_const_get(mrb, sym);
      ERR_PC_CLR(mrb);
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETCONST, BB) {
      mrb_vm_const_set(mrb, syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETMCNST, BB) {
      mrb_value val;

      ERR_PC_SET(mrb);
      val = mrb_const_get(mrb, regs[a], syms[b]);
      ERR_PC_CLR(mrb);
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETMCNST, BB) {
      mrb_const_set(mrb, regs[a+1], syms[b], regs[a]);
      NEXT;
    }

    CASE(OP_GETUPVAR, BBB) {
      mrb_value *regs_a = regs + a;
      struct REnv *e = uvenv(mrb, c);

      if (e && b < MRB_ENV_STACK_LEN(e)) {
        *regs_a = e->stack[b];
      }
      else {
        *regs_a = mrb_nil_value();
      }
      NEXT;
    }

    CASE(OP_SETUPVAR, BBB) {
      struct REnv *e = uvenv(mrb, c);

      if (e) {
        mrb_value *regs_a = regs + a;

        if (b < MRB_ENV_STACK_LEN(e)) {
          e->stack[b] = *regs_a;
          mrb_write_barrier(mrb, (struct RBasic*)e);
        }
      }
      NEXT;
    }

    CASE(OP_JMP, S) {
      pc = irep->iseq+a;
      JUMP;
    }
    CASE(OP_JMPIF, BS) {
      if (mrb_test(regs[a])) {
        pc = irep->iseq+b;
        JUMP;
      }
      NEXT;
    }
    CASE(OP_JMPNOT, BS) {
      if (!mrb_test(regs[a])) {
        pc = irep->iseq+b;
        JUMP;
      }
      NEXT;
    }
    CASE(OP_JMPNIL, BS) {
      if (mrb_nil_p(regs[a])) {
        pc = irep->iseq+b;
        JUMP;
      }
      NEXT;
    }

    CASE(OP_ONERR, S) {
      /* check rescue stack */
      if (mrb->c->ci->ridx == UINT16_MAX-1) {
        mrb_value exc = mrb_exc_new_str_lit(mrb, E_RUNTIME_ERROR, "too many nested rescues");
        mrb_exc_set(mrb, exc);
        goto L_RAISE;
      }
      /* expand rescue stack */
      if (mrb->c->rsize <= mrb->c->ci->ridx) {
        if (mrb->c->rsize == 0) mrb->c->rsize = RESCUE_STACK_INIT_SIZE;
        else {
          mrb->c->rsize *= 2;
          if (mrb->c->rsize <= mrb->c->ci->ridx) {
            mrb->c->rsize = UINT16_MAX;
          }
        }
        mrb->c->rescue = (uint16_t*)mrb_realloc(mrb, mrb->c->rescue, sizeof(uint16_t)*mrb->c->rsize);
      }
      /* push rescue stack */
      mrb->c->rescue[mrb->c->ci->ridx++] = a;
      NEXT;
    }

    CASE(OP_EXCEPT, B) {
      mrb_value exc = mrb_obj_value(mrb->exc);
      mrb->exc = 0;
      regs[a] = exc;
      NEXT;
    }
    CASE(OP_RESCUE, BB) {
      mrb_value exc = regs[a];  /* exc on stack */
      mrb_value e = regs[b];
      struct RClass *ec;

      switch (mrb_type(e)) {
      case MRB_TT_CLASS:
      case MRB_TT_MODULE:
        break;
      default:
        {
          mrb_value exc;

          exc = mrb_exc_new_str_lit(mrb, E_TYPE_ERROR,
                                    "class or module required for rescue clause");
          mrb_exc_set(mrb, exc);
          goto L_RAISE;
        }
      }
      ec = mrb_class_ptr(e);
      regs[b] = mrb_bool_value(mrb_obj_is_kind_of(mrb, exc, ec));
      NEXT;
    }

    CASE(OP_POPERR, B) {
      mrb->c->ci->ridx -= a;
      NEXT;
    }

    CASE(OP_RAISE, B) {
      mrb_exc_set(mrb, regs[a]);
      goto L_RAISE;
    }

    CASE(OP_EPUSH, B) {
      struct RProc *p;

      p = mrb_closure_new(mrb, irep->reps[a]);
      /* check ensure stack */
      if (mrb->c->eidx == UINT16_MAX-1) {
        mrb_value exc = mrb_exc_new_str_lit(mrb, E_RUNTIME_ERROR, "too many nested ensures");
        mrb_exc_set(mrb, exc);
        goto L_RAISE;
      }
      /* expand ensure stack */
      if (mrb->c->esize <= mrb->c->eidx+1) {
        if (mrb->c->esize == 0) mrb->c->esize = ENSURE_STACK_INIT_SIZE;
        else {
          mrb->c->esize *= 2;
          if (mrb->c->esize <= mrb->c->eidx) {
            mrb->c->esize = UINT16_MAX;
          }
        }
        mrb->c->ensure = (struct RProc**)mrb_realloc(mrb, mrb->c->ensure, sizeof(struct RProc*)*mrb->c->esize);
      }
      /* push ensure stack */
      mrb->c->ensure[mrb->c->eidx++] = p;
      mrb->c->ensure[mrb->c->eidx] = NULL;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_EPOP, B) {
      mrb_callinfo *ci = mrb->c->ci;
      unsigned int n, epos = ci->epos;
      mrb_value self = regs[0];
      struct RClass *target_class = ci->target_class;

      if (mrb->c->eidx <= epos) {
        NEXT;
      }

      if (a > (int)mrb->c->eidx - epos)
        a = mrb->c->eidx - epos;
      for (n=0; n<a; n++) {
        int nregs = irep->nregs;

        proc = mrb->c->ensure[epos+n];
        mrb->c->ensure[epos+n] = NULL;
        if (proc == NULL) continue;
        irep = proc->body.irep;
        ci = cipush(mrb);
        ci->mid = ci[-1].mid;
        ci->argc = 0;
        ci->proc = proc;
        ci->stackent = mrb->c->stack;
        ci->target_class = target_class;
        ci->pc = pc;
        ci->acc = nregs;
        mrb->c->stack += ci->acc;
        mrb_stack_extend(mrb, irep->nregs);
        regs[0] = self;
        pc = irep->iseq;
      }
      pool = irep->pool;
      syms = irep->syms;
      mrb->c->eidx = epos;
      JUMP;
    }

    CASE(OP_SENDV, BB) {
      c = CALL_MAXARGS;
      goto L_SEND;
    };

    CASE(OP_SENDVB, BB) {
      c = CALL_MAXARGS;
      goto L_SENDB;
    };

    CASE(OP_SEND, BBB)
    L_SEND:
    {
      /* push nil after arguments */
      int bidx = (c == CALL_MAXARGS) ? a+2 : a+c+1;
      SET_NIL_VALUE(regs[bidx]);
      goto L_SENDB;
    };
    L_SEND_SYM:
    {
      /* push nil after arguments */
      int bidx = (c == CALL_MAXARGS) ? a+2 : a+c+1;
      SET_NIL_VALUE(regs[bidx]);
      goto L_SENDB_SYM;
    };

    CASE(OP_SENDB, BBB)
    L_SENDB:
    mid = syms[b];
    L_SENDB_SYM:
    {
      int argc = (c == CALL_MAXARGS) ? -1 : c;
      int bidx = (argc < 0) ? a+2 : a+c+1;
      mrb_method_t m;
      struct RClass *cls;
      mrb_callinfo *ci = mrb->c->ci;
      mrb_value recv, blk;

      mrb_assert(bidx < irep->nregs);

      recv = regs[a];
      blk = regs[bidx];
      if (!mrb_nil_p(blk) && !mrb_proc_p(blk)) {
        blk = mrb_convert_type(mrb, blk, MRB_TT_PROC, "Proc", "to_proc");
        /* The stack might have been reallocated during mrb_convert_type(),
           see #3622 */
        regs[bidx] = blk;
      }
      cls = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &cls, mid);
      if (MRB_METHOD_UNDEF_P(m)) {
        mrb_sym missing = mrb_intern_lit(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &cls, missing);
        if (MRB_METHOD_UNDEF_P(m) || (missing == mrb->c->ci->mid && mrb_obj_eq(mrb, regs[0], recv))) {
          mrb_value args = (argc < 0) ? regs[a+1] : mrb_ary_new_from_values(mrb, c, regs+a+1);
          ERR_PC_SET(mrb);
          mrb_method_missing(mrb, mid, recv, args);
        }
        if (argc >= 0) {
          if (a+2 >= irep->nregs) {
            mrb_stack_extend(mrb, a+3);
          }
          regs[a+1] = mrb_ary_new_from_values(mrb, c, regs+a+1);
          regs[a+2] = blk;
          argc = -1;
        }
        mrb_ary_unshift(mrb, regs[a+1], mrb_symbol_value(mid));
        mid = missing;
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->stackent = mrb->c->stack;
      ci->target_class = cls;
      ci->argc = argc;

      ci->pc = pc;
      ci->acc = a;

      /* prepare stack */
      mrb->c->stack += a;

      if (MRB_METHOD_CFUNC_P(m)) {
        if (MRB_METHOD_PROC_P(m)) {
          struct RProc *p = MRB_METHOD_PROC(m);

          ci->proc = p;
          recv = p->body.func(mrb, recv);
        }
        else if (MRB_METHOD_NOARG_P(m) &&
                 (argc > 0 || (argc == -1 && RARRAY_LEN(regs[1]) != 0))) {
          argnum_error(mrb, 0);
          goto L_RAISE;
        }
        else {
          recv = MRB_METHOD_FUNC(m)(mrb, recv);
        }
        mrb_gc_arena_restore(mrb, ai);
        mrb_gc_arena_shrink(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        ci = mrb->c->ci;
        if (mrb_proc_p(blk)) {
          struct RProc *p = mrb_proc_ptr(blk);
          if (p && !MRB_PROC_STRICT_P(p) && MRB_PROC_ENV(p) == ci[-1].env) {
            p->flags |= MRB_PROC_ORPHAN;
          }
        }
        if (!ci->target_class) { /* return from context modifying method (resume/yield) */
          if (ci->acc == CI_ACC_RESUMED) {
            mrb->jmp = prev_jmp;
            return recv;
          }
          else {
            mrb_assert(!MRB_PROC_CFUNC_P(ci[-1].proc));
            proc = ci[-1].proc;
            irep = proc->body.irep;
            pool = irep->pool;
            syms = irep->syms;
          }
        }
        mrb->c->stack[0] = recv;
        /* pop stackpos */
        mrb->c->stack = ci->stackent;
        pc = ci->pc;
        cipop(mrb);
        JUMP;
      }
      else {
        /* setup environment for calling method */
        proc = ci->proc = MRB_METHOD_PROC(m);
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        mrb_stack_extend(mrb, (argc < 0 && irep->nregs < 3) ? 3 : irep->nregs);
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_CALL, Z) {
      mrb_callinfo *ci;
      mrb_value recv = mrb->c->stack[0];
      struct RProc *m = mrb_proc_ptr(recv);

      /* replace callinfo */
      ci = mrb->c->ci;
      ci->target_class = MRB_PROC_TARGET_CLASS(m);
      ci->proc = m;
      if (MRB_PROC_ENV_P(m)) {
        struct REnv *e = MRB_PROC_ENV(m);

        ci->mid = e->mid;
        if (!e->stack) {
          e->stack = mrb->c->stack;
        }
      }

      /* prepare stack */
      if (MRB_PROC_CFUNC_P(m)) {
        recv = MRB_PROC_CFUNC(m)(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        mrb_gc_arena_shrink(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        ci = mrb->c->ci;
        mrb->c->stack = ci->stackent;
        regs[ci->acc] = recv;
        pc = ci->pc;
        cipop(mrb);
        irep = mrb->c->ci->proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        JUMP;
      }
      else {
        /* setup environment for calling method */
        proc = m;
        irep = m->body.irep;
        if (!irep) {
          mrb->c->stack[0] = mrb_nil_value();
          a = 0;
          c = OP_R_NORMAL;
          goto L_OP_RETURN_BODY;
        }
        pool = irep->pool;
        syms = irep->syms;
        mrb_stack_extend(mrb, irep->nregs);
        if (ci->argc < 0) {
          if (irep->nregs > 3) {
            stack_clear(regs+3, irep->nregs-3);
          }
        }
        else if (ci->argc+2 < irep->nregs) {
          stack_clear(regs+ci->argc+2, irep->nregs-ci->argc-2);
        }
        if (MRB_PROC_ENV_P(m)) {
          regs[0] = MRB_PROC_ENV(m)->stack[0];
        }
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_SUPER, BB) {
      int argc = (b == CALL_MAXARGS) ? -1 : b;
      int bidx = (argc < 0) ? a+2 : a+b+1;
      mrb_method_t m;
      struct RClass *cls;
      mrb_callinfo *ci = mrb->c->ci;
      mrb_value recv, blk;
      struct RProc *p = ci->proc;
      mrb_sym mid = ci->mid;
      struct RClass* target_class = MRB_PROC_TARGET_CLASS(p);

      if (MRB_PROC_ENV_P(p) && p->e.env->mid && p->e.env->mid != mid) { /* alias support */
        mid = p->e.env->mid;    /* restore old mid */
      }
      mrb_assert(bidx < irep->nregs);

      if (mid == 0 || !target_class) {
        mrb_value exc = mrb_exc_new_str_lit(mrb, E_NOMETHOD_ERROR, "super called outside of method");
        mrb_exc_set(mrb, exc);
        goto L_RAISE;
      }
      if (target_class->tt == MRB_TT_MODULE) {
        target_class = ci->target_class;
        if (target_class->tt != MRB_TT_ICLASS) {
          mrb_value exc = mrb_exc_new_str_lit(mrb, E_RUNTIME_ERROR, "superclass info lost [mruby limitations]");
          mrb_exc_set(mrb, exc);
          goto L_RAISE;
        }
      }
      recv = regs[0];
      if (!mrb_obj_is_kind_of(mrb, recv, target_class)) {
        mrb_value exc = mrb_exc_new_str_lit(mrb, E_TYPE_ERROR,
                                            "self has wrong type to call super in this context");
        mrb_exc_set(mrb, exc);
        goto L_RAISE;
      }
      blk = regs[bidx];
      if (!mrb_nil_p(blk) && !mrb_proc_p(blk)) {
        blk = mrb_convert_type(mrb, blk, MRB_TT_PROC, "Proc", "to_proc");
        /* The stack or ci stack might have been reallocated during
           mrb_convert_type(), see #3622 and #3784 */
        regs[bidx] = blk;
        ci = mrb->c->ci;
      }
      cls = target_class->super;
      m = mrb_method_search_vm(mrb, &cls, mid);
      if (MRB_METHOD_UNDEF_P(m)) {
        mrb_sym missing = mrb_intern_lit(mrb, "method_missing");

        if (mid != missing) {
          cls = mrb_class(mrb, recv);
        }
        m = mrb_method_search_vm(mrb, &cls, missing);
        if (MRB_METHOD_UNDEF_P(m)) {
          mrb_value args = (argc < 0) ? regs[a+1] : mrb_ary_new_from_values(mrb, b, regs+a+1);
          ERR_PC_SET(mrb);
          mrb_method_missing(mrb, mid, recv, args);
        }
        mid = missing;
        if (argc >= 0) {
          if (a+2 >= irep->nregs) {
            mrb_stack_extend(mrb, a+3);
          }
          regs[a+1] = mrb_ary_new_from_values(mrb, b, regs+a+1);
          regs[a+2] = blk;
          argc = -1;
        }
        mrb_ary_unshift(mrb, regs[a+1], mrb_symbol_value(ci->mid));
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->stackent = mrb->c->stack;
      ci->target_class = cls;
      ci->pc = pc;
      ci->argc = argc;

      /* prepare stack */
      mrb->c->stack += a;
      mrb->c->stack[0] = recv;

      if (MRB_METHOD_CFUNC_P(m)) {
        mrb_value v;

        if (MRB_METHOD_PROC_P(m)) {
          ci->proc = MRB_METHOD_PROC(m);
        }
        v = MRB_METHOD_CFUNC(m)(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        ci = mrb->c->ci;
        if (!ci->target_class) { /* return from context modifying method (resume/yield) */
          if (ci->acc == CI_ACC_RESUMED) {
            mrb->jmp = prev_jmp;
            return v;
          }
          else {
            mrb_assert(!MRB_PROC_CFUNC_P(ci[-1].proc));
            proc = ci[-1].proc;
            irep = proc->body.irep;
            pool = irep->pool;
            syms = irep->syms;
          }
        }
        mrb->c->stack[0] = v;
        /* pop stackpos */
        mrb->c->stack = ci->stackent;
        pc = ci->pc;
        cipop(mrb);
        JUMP;
      }
      else {
        /* fill callinfo */
        ci->acc = a;

        /* setup environment for calling method */
        proc = ci->proc = MRB_METHOD_PROC(m);
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        mrb_stack_extend(mrb, (argc < 0 && irep->nregs < 3) ? 3 : irep->nregs);
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_ARGARY, BS) {
      int m1 = (b>>11)&0x3f;
      int r  = (b>>10)&0x1;
      int m2 = (b>>5)&0x1f;
      int kd = (b>>4)&0x1;
      int lv = (b>>0)&0xf;
      mrb_value *stack;

      if (mrb->c->ci->mid == 0 || mrb->c->ci->target_class == NULL) {
        mrb_value exc;

      L_NOSUPER:
        exc = mrb_exc_new_str_lit(mrb, E_NOMETHOD_ERROR, "super called outside of method");
        mrb_exc_set(mrb, exc);
        goto L_RAISE;
      }
      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        if (!e) goto L_NOSUPER;
        if (MRB_ENV_STACK_LEN(e) <= m1+r+m2+kd+1)
          goto L_NOSUPER;
        stack = e->stack + 1;
      }
      if (r == 0) {
        regs[a] = mrb_ary_new_from_values(mrb, m1+m2+kd, stack);
      }
      else {
        mrb_value *pp = NULL;
        struct RArray *rest;
        int len = 0;

        if (mrb_array_p(stack[m1])) {
          struct RArray *ary = mrb_ary_ptr(stack[m1]);

          pp = ARY_PTR(ary);
          len = (int)ARY_LEN(ary);
        }
        regs[a] = mrb_ary_new_capa(mrb, m1+len+m2+kd);
        rest = mrb_ary_ptr(regs[a]);
        if (m1 > 0) {
          stack_copy(ARY_PTR(rest), stack, m1);
        }
        if (len > 0) {
          stack_copy(ARY_PTR(rest)+m1, pp, len);
        }
        if (m2 > 0) {
          stack_copy(ARY_PTR(rest)+m1+len, stack+m1+1, m2);
        }
        if (kd) {
          stack_copy(ARY_PTR(rest)+m1+len+m2, stack+m1+m2+1, kd);
        }
        ARY_SET_LEN(rest, m1+len+m2+kd);
      }
      regs[a+1] = stack[m1+r+m2];
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ENTER, W) {
      int m1 = MRB_ASPEC_REQ(a);
      int o  = MRB_ASPEC_OPT(a);
      int r  = MRB_ASPEC_REST(a);
      int m2 = MRB_ASPEC_POST(a);
      int kd = (MRB_ASPEC_KEY(a) > 0 || MRB_ASPEC_KDICT(a))? 1 : 0;
      /* unused
      int b  = MRB_ASPEC_BLOCK(a);
      */
      int argc = mrb->c->ci->argc;
      mrb_value *argv = regs+1;
      mrb_value * const argv0 = argv;
      int const len = m1 + o + r + m2;
      int const blk_pos = len + kd + 1;
      mrb_value *blk = &argv[argc < 0 ? 1 : argc];
      mrb_value kdict;
      int kargs = kd;

      /* arguments is passed with Array */
      if (argc < 0) {
        struct RArray *ary = mrb_ary_ptr(regs[1]);
        argv = ARY_PTR(ary);
        argc = (int)ARY_LEN(ary);
        mrb_gc_protect(mrb, regs[1]);
      }

      /* strict argument check */
      if (mrb->c->ci->proc && MRB_PROC_STRICT_P(mrb->c->ci->proc)) {
        if (argc < m1 + m2 || (r == 0 && argc > len + kd)) {
          argnum_error(mrb, m1+m2);
          goto L_RAISE;
        }
      }
      /* extract first argument array to arguments */
      else if (len > 1 && argc == 1 && mrb_array_p(argv[0])) {
        mrb_gc_protect(mrb, argv[0]);
        argc = (int)RARRAY_LEN(argv[0]);
        argv = RARRAY_PTR(argv[0]);
      }

      if (kd) {
        /* check last arguments is hash if method takes keyword arguments */
        if (argc == m1+m2) {
          kdict = mrb_hash_new(mrb);
          kargs = 0;
        }
        else {
          if (argv && argc > 0 && mrb_hash_p(argv[argc-1])) {
            kdict = argv[argc-1];
            mrb_hash_check_kdict(mrb, kdict);
          }
          else if (r || argc <= m1+m2+o
                   || !(mrb->c->ci->proc && MRB_PROC_STRICT_P(mrb->c->ci->proc))) {
            kdict = mrb_hash_new(mrb);
            kargs = 0;
          }
          else {
            argnum_error(mrb, m1+m2);
            goto L_RAISE;
          }
          if (MRB_ASPEC_KEY(a) > 0) {
            kdict = mrb_hash_dup(mrb, kdict);
          }
        }
      }

      /* no rest arguments */
      if (argc-kargs < len) {
        int mlen = m2;
        if (argc < m1+m2) {
          mlen = m1 < argc ? argc - m1 : 0;
        }
        regs[blk_pos] = *blk; /* move block */
        if (kd) regs[len + 1] = kdict;

        /* copy mandatory and optional arguments */
        if (argv0 != argv) {
          value_move(&regs[1], argv, argc-mlen); /* m1 + o */
        }
        if (argc < m1) {
          stack_clear(&regs[argc+1], m1-argc);
        }
        /* copy post mandatory arguments */
        if (mlen) {
          value_move(&regs[len-m2+1], &argv[argc-mlen], mlen);
        }
        if (mlen < m2) {
          stack_clear(&regs[len-m2+mlen+1], m2-mlen);
        }
        /* initalize rest arguments with empty Array */
        if (r) {
          regs[m1+o+1] = mrb_ary_new_capa(mrb, 0);
        }
        /* skip initailizer of passed arguments */
        if (o > 0 && argc-kargs > m1+m2)
          pc += (argc - kargs - m1 - m2)*3;
      }
      else {
        int rnum = 0;
        if (argv0 != argv) {
          regs[blk_pos] = *blk; /* move block */
          if (kd) regs[len + 1] = kdict;
          value_move(&regs[1], argv, m1+o);
        }
        if (r) {
          mrb_value ary;

          rnum = argc-m1-o-m2-kargs;
          ary = mrb_ary_new_from_values(mrb, rnum, argv+m1+o);
          regs[m1+o+1] = ary;
        }
        if (m2) {
          if (argc-m2 > m1) {
            value_move(&regs[m1+o+r+1], &argv[m1+o+rnum], m2);
          }
        }
        if (argv0 == argv) {
          regs[blk_pos] = *blk; /* move block */
          if (kd) regs[len + 1] = kdict;
        }
        pc += o*3;
      }

      /* format arguments for generated code */
      mrb->c->ci->argc = len + kd;

      /* clear local (but non-argument) variables */
      if (irep->nlocals-blk_pos-1 > 0) {
        stack_clear(&regs[blk_pos+1], irep->nlocals-blk_pos-1);
      }
      JUMP;
    }

    CASE(OP_KARG, BB) {
      mrb_value k = mrb_symbol_value(syms[b]);
      mrb_value kdict = regs[mrb->c->ci->argc];

      if (!mrb_hash_p(kdict) || !mrb_hash_key_p(mrb, kdict, k)) {
        mrb_value str = mrb_format(mrb, "missing keyword: %v", k);
        mrb_exc_set(mrb, mrb_exc_new_str(mrb, E_ARGUMENT_ERROR, str));
        goto L_RAISE;
      }
      regs[a] = mrb_hash_get(mrb, kdict, k);
      mrb_hash_delete_key(mrb, kdict, k);
      NEXT;
    }

    CASE(OP_KEY_P, BB) {
      mrb_value k = mrb_symbol_value(syms[b]);
      mrb_value kdict = regs[mrb->c->ci->argc];
      mrb_bool key_p = FALSE;

      if (mrb_hash_p(kdict)) {
        key_p = mrb_hash_key_p(mrb, kdict, k);
      }
      regs[a] = mrb_bool_value(key_p);
      NEXT;
    }

    CASE(OP_KEYEND, Z) {
      mrb_value kdict = regs[mrb->c->ci->argc];

      if (mrb_hash_p(kdict) && !mrb_hash_empty_p(mrb, kdict)) {
        mrb_value keys = mrb_hash_keys(mrb, kdict);
        mrb_value key1 = RARRAY_PTR(keys)[0];
        mrb_value str = mrb_format(mrb, "unknown keyword: %v", key1);
        mrb_exc_set(mrb, mrb_exc_new_str(mrb, E_ARGUMENT_ERROR, str));
        goto L_RAISE;
      }
      NEXT;
    }

    CASE(OP_BREAK, B) {
      c = OP_R_BREAK;
      goto L_RETURN;
    }
    CASE(OP_RETURN_BLK, B) {
      c = OP_R_RETURN;
      goto L_RETURN;
    }
    CASE(OP_RETURN, B)
    c = OP_R_NORMAL;
    L_RETURN:
    {
       mrb_callinfo *ci;

#define ecall_adjust() do {\
  ptrdiff_t cioff = ci - mrb->c->cibase;\
  ecall(mrb);\
  ci = mrb->c->cibase + cioff;\
} while (0)

      ci = mrb->c->ci;
      if (ci->mid) {
        mrb_value blk;

        if (ci->argc < 0) {
          blk = regs[2];
        }
        else {
          blk = regs[ci->argc+1];
        }
        if (mrb_proc_p(blk)) {
          struct RProc *p = mrb_proc_ptr(blk);

          if (!MRB_PROC_STRICT_P(p) &&
              ci > mrb->c->cibase && MRB_PROC_ENV(p) == ci[-1].env) {
            p->flags |= MRB_PROC_ORPHAN;
          }
        }
      }

      if (mrb->exc) {
        mrb_callinfo *ci0;

      L_RAISE:
        ci0 = ci = mrb->c->ci;
        if (ci == mrb->c->cibase) {
          if (ci->ridx == 0) goto L_FTOP;
          goto L_RESCUE;
        }
        while (ci[0].ridx == ci[-1].ridx) {
          cipop(mrb);
          mrb->c->stack = ci->stackent;
          if (ci->acc == CI_ACC_SKIP && prev_jmp) {
            mrb->jmp = prev_jmp;
            MRB_THROW(prev_jmp);
          }
          ci = mrb->c->ci;
          if (ci == mrb->c->cibase) {
            if (ci->ridx == 0) {
            L_FTOP:             /* fiber top */
              if (mrb->c == mrb->root_c) {
                mrb->c->stack = mrb->c->stbase;
                goto L_STOP;
              }
              else {
                struct mrb_context *c = mrb->c;

                while (c->eidx > ci->epos) {
                  ecall_adjust();
                }
                c->status = MRB_FIBER_TERMINATED;
                mrb->c = c->prev;
                c->prev = NULL;
                goto L_RAISE;
              }
            }
            break;
          }
          /* call ensure only when we skip this callinfo */
          if (ci[0].ridx == ci[-1].ridx) {
            while (mrb->c->eidx > ci->epos) {
              ecall_adjust();
            }
          }
        }
      L_RESCUE:
        if (ci->ridx == 0) goto L_STOP;
        proc = ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        if (ci < ci0) {
          mrb->c->stack = ci[1].stackent;
        }
        mrb_stack_extend(mrb, irep->nregs);
        pc = irep->iseq+mrb->c->rescue[--ci->ridx];
      }
      else {
        int acc;
        mrb_value v;
        struct RProc *dst;

        ci = mrb->c->ci;
        v = regs[a];
        mrb_gc_protect(mrb, v);
        switch (c) {
        case OP_R_RETURN:
          /* Fall through to OP_R_NORMAL otherwise */
          if (ci->acc >=0 && MRB_PROC_ENV_P(proc) && !MRB_PROC_STRICT_P(proc)) {
            mrb_callinfo *cibase = mrb->c->cibase;
            dst = top_proc(mrb, proc);

            if (MRB_PROC_ENV_P(dst)) {
              struct REnv *e = MRB_PROC_ENV(dst);

              if (!MRB_ENV_STACK_SHARED_P(e) || (e->cxt && e->cxt != mrb->c)) {
                localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
                goto L_RAISE;
              }
            }
            while (cibase <= ci && ci->proc != dst) {
              if (ci->acc < 0) {
                localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
                goto L_RAISE;
              }
              ci--;
            }
            if (ci <= cibase) {
              localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
              goto L_RAISE;
            }
            break;
          }
          /* fallthrough */
        case OP_R_NORMAL:
        NORMAL_RETURN:
          if (ci == mrb->c->cibase) {
            struct mrb_context *c = mrb->c;

            if (!c->prev) { /* toplevel return */
              regs[irep->nlocals] = v;
              goto L_STOP;
            }
            if (c->prev->ci == c->prev->cibase) {
              mrb_value exc = mrb_exc_new_str_lit(mrb, E_FIBER_ERROR, "double resume");
              mrb_exc_set(mrb, exc);
              goto L_RAISE;
            }
            while (c->eidx > 0) {
              ecall(mrb);
            }
            /* automatic yield at the end */
            c->status = MRB_FIBER_TERMINATED;
            mrb->c = c->prev;
            c->prev = NULL;
            mrb->c->status = MRB_FIBER_RUNNING;
            ci = mrb->c->ci;
          }
          break;
        case OP_R_BREAK:
          if (MRB_PROC_STRICT_P(proc)) goto NORMAL_RETURN;
          if (MRB_PROC_ORPHAN_P(proc)) {
            mrb_value exc;

          L_BREAK_ERROR:
            exc = mrb_exc_new_str_lit(mrb, E_LOCALJUMP_ERROR,
                                      "break from proc-closure");
            mrb_exc_set(mrb, exc);
            goto L_RAISE;
          }
          if (!MRB_PROC_ENV_P(proc) || !MRB_ENV_STACK_SHARED_P(MRB_PROC_ENV(proc))) {
            goto L_BREAK_ERROR;
          }
          else {
            struct REnv *e = MRB_PROC_ENV(proc);

            if (e->cxt != mrb->c) {
              goto L_BREAK_ERROR;
            }
          }
          while (mrb->c->eidx > mrb->c->ci->epos) {
            ecall_adjust();
          }
          /* break from fiber block */
          if (ci == mrb->c->cibase && ci->pc) {
            struct mrb_context *c = mrb->c;

            mrb->c = c->prev;
            c->prev = NULL;
            ci = mrb->c->ci;
          }
          if (ci->acc < 0) {
            mrb_gc_arena_restore(mrb, ai);
            mrb->c->vmexec = FALSE;
            mrb->exc = (struct RObject*)break_new(mrb, proc, v);
            mrb->jmp = prev_jmp;
            MRB_THROW(prev_jmp);
          }
          if (FALSE) {
          L_BREAK:
            v = mrb_break_value_get((struct RBreak*)mrb->exc);
            proc = mrb_break_proc_get((struct RBreak*)mrb->exc);
            mrb->exc = NULL;
            ci = mrb->c->ci;
          }
          mrb->c->stack = ci->stackent;
          proc = proc->upper;
          while (mrb->c->cibase < ci &&  ci[-1].proc != proc) {
            if (ci[-1].acc == CI_ACC_SKIP) {
              while (ci < mrb->c->ci) {
                cipop(mrb);
              }
              goto L_BREAK_ERROR;
            }
            ci--;
          }
          if (ci == mrb->c->cibase) {
            goto L_BREAK_ERROR;
          }
          break;
        default:
          /* cannot happen */
          break;
        }
        while (ci < mrb->c->ci) {
          cipop(mrb);
        }
        ci[0].ridx = ci[-1].ridx;
        while (mrb->c->eidx > ci->epos) {
          ecall_adjust();
        }
        if (mrb->c->vmexec && !ci->target_class) {
          mrb_gc_arena_restore(mrb, ai);
          mrb->c->vmexec = FALSE;
          mrb->jmp = prev_jmp;
          return v;
        }
        acc = ci->acc;
        mrb->c->stack = ci->stackent;
        cipop(mrb);
        if (acc == CI_ACC_SKIP || acc == CI_ACC_DIRECT) {
          mrb_gc_arena_restore(mrb, ai);
          mrb->jmp = prev_jmp;
          return v;
        }
        pc = ci->pc;
        ci = mrb->c->ci;
        DEBUG(fprintf(stderr, "from :%s\n", mrb_sym_name(mrb, ci->mid)));
        proc = mrb->c->ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;

        regs[acc] = v;
        mrb_gc_arena_restore(mrb, ai);
      }
      JUMP;
    }

    CASE(OP_BLKPUSH, BS) {
      int m1 = (b>>11)&0x3f;
      int r  = (b>>10)&0x1;
      int m2 = (b>>5)&0x1f;
      int kd = (b>>4)&0x1;
      int lv = (b>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        if (!e || (!MRB_ENV_STACK_SHARED_P(e) && e->mid == 0) ||
            MRB_ENV_STACK_LEN(e) <= m1+r+m2+1) {
          localjump_error(mrb, LOCALJUMP_ERROR_YIELD);
          goto L_RAISE;
        }
        stack = e->stack + 1;
      }
      if (mrb_nil_p(stack[m1+r+m2])) {
        localjump_error(mrb, LOCALJUMP_ERROR_YIELD);
        goto L_RAISE;
      }
      regs[a] = stack[m1+r+m2+kd];
      NEXT;
    }

#define TYPES2(a,b) ((((uint16_t)(a))<<8)|(((uint16_t)(b))&0xff))
#define OP_MATH(op_name)                                                    \
  /* need to check if op is overridden */                                   \
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {                  \
    OP_MATH_CASE_FIXNUM(op_name);                                           \
    OP_MATH_CASE_FLOAT(op_name, fixnum, float);                             \
    OP_MATH_CASE_FLOAT(op_name, float,  fixnum);                            \
    OP_MATH_CASE_FLOAT(op_name, float,  float);                             \
    OP_MATH_CASE_STRING_##op_name();                                        \
    default:                                                                \
      c = 1;                                                                \
      mid = mrb_intern_lit(mrb, MRB_STRINGIZE(OP_MATH_OP_##op_name));       \
      goto L_SEND_SYM;                                                      \
  }                                                                         \
  NEXT;
#define OP_MATH_CASE_FIXNUM(op_name)                                        \
  case TYPES2(MRB_TT_FIXNUM, MRB_TT_FIXNUM):                                \
    {                                                                       \
      mrb_int x = mrb_fixnum(regs[a]), y = mrb_fixnum(regs[a+1]), z;        \
      if (mrb_int_##op_name##_overflow(x, y, &z))                           \
        OP_MATH_OVERFLOW_INT(op_name, x, y, z);                             \
      else                                                                  \
        SET_INT_VALUE(regs[a], z);                                          \
    }                                                                       \
    break
#ifdef MRB_WITHOUT_FLOAT
#define OP_MATH_CASE_FLOAT(op_name, t1, t2) (void)0
#define OP_MATH_OVERFLOW_INT(op_name, x, y, z) SET_INT_VALUE(regs[a], z)
#else
#define OP_MATH_CASE_FLOAT(op_name, t1, t2)                                     \
  case TYPES2(OP_MATH_TT_##t1, OP_MATH_TT_##t2):                                \
    {                                                                           \
      mrb_float z = mrb_##t1(regs[a]) OP_MATH_OP_##op_name mrb_##t2(regs[a+1]); \
      SET_FLOAT_VALUE(mrb, regs[a], z);                                         \
    }                                                                           \
    break
#define OP_MATH_OVERFLOW_INT(op_name, x, y, z) \
  SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x OP_MATH_OP_##op_name (mrb_float)y)
#endif
#define OP_MATH_CASE_STRING_add()                                           \
  case TYPES2(MRB_TT_STRING, MRB_TT_STRING):                                \
    regs[a] = mrb_str_plus(mrb, regs[a], regs[a+1]);                        \
    mrb_gc_arena_restore(mrb, ai);                                          \
    break
#define OP_MATH_CASE_STRING_sub() (void)0
#define OP_MATH_CASE_STRING_mul() (void)0
#define OP_MATH_OP_add +
#define OP_MATH_OP_sub -
#define OP_MATH_OP_mul *
#define OP_MATH_TT_fixnum MRB_TT_FIXNUM
#define OP_MATH_TT_float  MRB_TT_FLOAT

    CASE(OP_ADD, B) {
      OP_MATH(add);
    }

    CASE(OP_SUB, B) {
      OP_MATH(sub);
    }

    CASE(OP_MUL, B) {
      OP_MATH(mul);
    }

    CASE(OP_DIV, B) {
#ifndef MRB_WITHOUT_FLOAT
      double x, y, f;
#endif

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
#ifdef MRB_WITHOUT_FLOAT
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_INT_VALUE(regs[a], y ? x / y : 0);
        }
        break;
#else
        x = (mrb_float)mrb_fixnum(regs[a]);
        y = (mrb_float)mrb_fixnum(regs[a+1]);
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        x = (mrb_float)mrb_fixnum(regs[a]);
        y = mrb_float(regs[a+1]);
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
        x = mrb_float(regs[a]);
        y = (mrb_float)mrb_fixnum(regs[a+1]);
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
        x = mrb_float(regs[a]);
        y = mrb_float(regs[a+1]);
        break;
#endif
      default:
        c = 1;
        mid = mrb_intern_lit(mrb, "/");
        goto L_SEND_SYM;
      }

#ifndef MRB_WITHOUT_FLOAT
      if (y == 0) {
        if (x > 0) f = INFINITY;
        else if (x < 0) f = -INFINITY;
        else /* if (x == 0) */ f = NAN;
      }
      else {
        f = x / y;
      }
      SET_FLOAT_VALUE(mrb, regs[a], f);
#endif
      NEXT;
    }

#define OP_MATHI(op_name)                                                   \
  /* need to check if op is overridden */                                   \
  switch (mrb_type(regs[a])) {                                              \
    OP_MATHI_CASE_FIXNUM(op_name);                                          \
    OP_MATHI_CASE_FLOAT(op_name);                                           \
    default:                                                                \
      SET_INT_VALUE(regs[a+1], b);                                          \
      c = 1;                                                                \
      mid = mrb_intern_lit(mrb, MRB_STRINGIZE(OP_MATH_OP_##op_name));       \
      goto L_SEND_SYM;                                                      \
  }                                                                         \
  NEXT;
#define OP_MATHI_CASE_FIXNUM(op_name)                                       \
  case MRB_TT_FIXNUM:                                                       \
    {                                                                       \
      mrb_int x = mrb_fixnum(regs[a]), y = (mrb_int)b, z;                   \
      if (mrb_int_##op_name##_overflow(x, y, &z))                           \
        OP_MATH_OVERFLOW_INT(op_name, x, y, z);                             \
      else                                                                  \
        SET_INT_VALUE(regs[a], z);                                          \
    }                                                                       \
    break
#ifdef MRB_WITHOUT_FLOAT
#define OP_MATHI_CASE_FLOAT(op_name) (void)0
#else
#define OP_MATHI_CASE_FLOAT(op_name)                                        \
  case MRB_TT_FLOAT:                                                        \
    {                                                                       \
      mrb_float z = mrb_float(regs[a]) OP_MATH_OP_##op_name b;              \
      SET_FLOAT_VALUE(mrb, regs[a], z);                                     \
    }                                                                       \
    break
#endif

    CASE(OP_ADDI, BB) {
      OP_MATHI(add);
    }

    CASE(OP_SUBI, BB) {
      OP_MATHI(sub);
    }

#define OP_CMP_BODY(op,v1,v2) (v1(regs[a]) op v2(regs[a+1]))

#ifdef MRB_WITHOUT_FLOAT
#define OP_CMP(op) do {\
  int result;\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    result = OP_CMP_BODY(op,mrb_fixnum,mrb_fixnum);\
    break;\
  default:\
    c = 1;\
    mid = mrb_intern_lit(mrb, # op);\
    goto L_SEND_SYM;\
  }\
  if (result) {\
    SET_TRUE_VALUE(regs[a]);\
  }\
  else {\
    SET_FALSE_VALUE(regs[a]);\
  }\
} while(0)
#else
#define OP_CMP(op) do {\
  int result;\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    result = OP_CMP_BODY(op,mrb_fixnum,mrb_fixnum);\
    break;\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):\
    result = OP_CMP_BODY(op,mrb_fixnum,mrb_float);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):\
    result = OP_CMP_BODY(op,mrb_float,mrb_fixnum);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):\
    result = OP_CMP_BODY(op,mrb_float,mrb_float);\
    break;\
  default:\
    c = 1;\
    mid = mrb_intern_lit(mrb, # op);\
    goto L_SEND_SYM;\
  }\
  if (result) {\
    SET_TRUE_VALUE(regs[a]);\
  }\
  else {\
    SET_FALSE_VALUE(regs[a]);\
  }\
} while(0)
#endif

    CASE(OP_EQ, B) {
      if (mrb_obj_eq(mrb, regs[a], regs[a+1])) {
        SET_TRUE_VALUE(regs[a]);
      }
      else {
        OP_CMP(==);
      }
      NEXT;
    }

    CASE(OP_LT, B) {
      OP_CMP(<);
      NEXT;
    }

    CASE(OP_LE, B) {
      OP_CMP(<=);
      NEXT;
    }

    CASE(OP_GT, B) {
      OP_CMP(>);
      NEXT;
    }

    CASE(OP_GE, B) {
      OP_CMP(>=);
      NEXT;
    }

    CASE(OP_ARRAY, BB) {
      mrb_value v = mrb_ary_new_from_values(mrb, b, &regs[a]);
      regs[a] = v;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }
    CASE(OP_ARRAY2, BBB) {
      mrb_value v = mrb_ary_new_from_values(mrb, c, &regs[b]);
      regs[a] = v;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYCAT, B) {
      mrb_value splat = mrb_ary_splat(mrb, regs[a+1]);
      if (mrb_nil_p(regs[a])) {
        regs[a] = splat;
      }
      else {
        mrb_ary_concat(mrb, regs[a], splat);
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYPUSH, B) {
      mrb_ary_push(mrb, regs[a], regs[a+1]);
      NEXT;
    }

    CASE(OP_ARYDUP, B) {
      mrb_value ary = regs[a];
      if (mrb_array_p(ary)) {
        ary = mrb_ary_new_from_values(mrb, RARRAY_LEN(ary), RARRAY_PTR(ary));
      }
      else {
        ary = mrb_ary_new_from_values(mrb, 1, &ary);
      }
      regs[a] = ary;
      NEXT;
    }

    CASE(OP_AREF, BBB) {
      mrb_value v = regs[b];

      if (!mrb_array_p(v)) {
        if (c == 0) {
          regs[a] = v;
        }
        else {
          SET_NIL_VALUE(regs[a]);
        }
      }
      else {
        v = mrb_ary_ref(mrb, v, c);
        regs[a] = v;
      }
      NEXT;
    }

    CASE(OP_ASET, BBB) {
      mrb_ary_set(mrb, regs[b], c, regs[a]);
      NEXT;
    }

    CASE(OP_APOST, BBB) {
      mrb_value v = regs[a];
      int pre  = b;
      int post = c;
      struct RArray *ary;
      int len, idx;

      if (!mrb_array_p(v)) {
        v = mrb_ary_new_from_values(mrb, 1, &regs[a]);
      }
      ary = mrb_ary_ptr(v);
      len = (int)ARY_LEN(ary);
      if (len > pre + post) {
        v = mrb_ary_new_from_values(mrb, len - pre - post, ARY_PTR(ary)+pre);
        regs[a++] = v;
        while (post--) {
          regs[a++] = ARY_PTR(ary)[len-post-1];
        }
      }
      else {
        v = mrb_ary_new_capa(mrb, 0);
        regs[a++] = v;
        for (idx=0; idx+pre<len; idx++) {
          regs[a+idx] = ARY_PTR(ary)[pre+idx];
        }
        while (idx < post) {
          SET_NIL_VALUE(regs[a+idx]);
          idx++;
        }
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_INTERN, B) {
      mrb_sym sym = mrb_intern_str(mrb, regs[a]);

      regs[a] = mrb_symbol_value(sym);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_STRING, BB) {
      mrb_value str = mrb_str_dup(mrb, pool[b]);

      regs[a] = str;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_STRCAT, B) {
      mrb_str_concat(mrb, regs[a], regs[a+1]);
      NEXT;
    }

    CASE(OP_HASH, BB) {
      mrb_value hash = mrb_hash_new_capa(mrb, b);
      int i;
      int lim = a+b*2;

      for (i=a; i<lim; i+=2) {
        mrb_hash_set(mrb, hash, regs[i], regs[i+1]);
      }
      regs[a] = hash;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_HASHADD, BB) {
      mrb_value hash;
      int i;
      int lim = a+b*2+1;

      hash = mrb_ensure_hash_type(mrb, regs[a]);
      for (i=a+1; i<lim; i+=2) {
        mrb_hash_set(mrb, hash, regs[i], regs[i+1]);
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }
    CASE(OP_HASHCAT, B) {
      mrb_value hash = mrb_ensure_hash_type(mrb, regs[a]);

      mrb_hash_merge(mrb, hash, regs[a+1]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_LAMBDA, BB)
    c = OP_L_LAMBDA;
    L_MAKE_LAMBDA:
    {
      struct RProc *p;
      mrb_irep *nirep = irep->reps[b];

      if (c & OP_L_CAPTURE) {
        p = mrb_closure_new(mrb, nirep);
      }
      else {
        p = mrb_proc_new(mrb, nirep);
        p->flags |= MRB_PROC_SCOPE;
      }
      if (c & OP_L_STRICT) p->flags |= MRB_PROC_STRICT;
      regs[a] = mrb_obj_value(p);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }
    CASE(OP_BLOCK, BB) {
      c = OP_L_BLOCK;
      goto L_MAKE_LAMBDA;
    }
    CASE(OP_METHOD, BB) {
      c = OP_L_METHOD;
      goto L_MAKE_LAMBDA;
    }

    CASE(OP_RANGE_INC, B) {
      mrb_value val = mrb_range_new(mrb, regs[a], regs[a+1], FALSE);
      regs[a] = val;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_RANGE_EXC, B) {
      mrb_value val = mrb_range_new(mrb, regs[a], regs[a+1], TRUE);
      regs[a] = val;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_OCLASS, B) {
      regs[a] = mrb_obj_value(mrb->object_class);
      NEXT;
    }

    CASE(OP_CLASS, BB) {
      struct RClass *c = 0, *baseclass;
      mrb_value base, super;
      mrb_sym id = syms[b];

      base = regs[a];
      super = regs[a+1];
      if (mrb_nil_p(base)) {
        baseclass = MRB_PROC_TARGET_CLASS(mrb->c->ci->proc);
        base = mrb_obj_value(baseclass);
      }
      c = mrb_vm_define_class(mrb, base, super, id);
      regs[a] = mrb_obj_value(c);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_MODULE, BB) {
      struct RClass *cls = 0, *baseclass;
      mrb_value base;
      mrb_sym id = syms[b];

      base = regs[a];
      if (mrb_nil_p(base)) {
        baseclass = MRB_PROC_TARGET_CLASS(mrb->c->ci->proc);
        base = mrb_obj_value(baseclass);
      }
      cls = mrb_vm_define_module(mrb, base, id);
      regs[a] = mrb_obj_value(cls);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_EXEC, BB) {
      mrb_callinfo *ci;
      mrb_value recv = regs[a];
      struct RProc *p;
      mrb_irep *nirep = irep->reps[b];

      /* prepare closure */
      p = mrb_proc_new(mrb, nirep);
      p->c = NULL;
      mrb_field_write_barrier(mrb, (struct RBasic*)p, (struct RBasic*)proc);
      MRB_PROC_SET_TARGET_CLASS(p, mrb_class_ptr(recv));
      p->flags |= MRB_PROC_SCOPE;

      /* prepare call stack */
      ci = cipush(mrb);
      ci->pc = pc;
      ci->acc = a;
      ci->mid = 0;
      ci->stackent = mrb->c->stack;
      ci->argc = 0;
      ci->target_class = mrb_class_ptr(recv);

      /* prepare stack */
      mrb->c->stack += a;

      /* setup block to call */
      ci->proc = p;

      irep = p->body.irep;
      pool = irep->pool;
      syms = irep->syms;
      mrb_stack_extend(mrb, irep->nregs);
      stack_clear(regs+1, irep->nregs-1);
      pc = irep->iseq;
      JUMP;
    }

    CASE(OP_DEF, BB) {
      struct RClass *target = mrb_class_ptr(regs[a]);
      struct RProc *p = mrb_proc_ptr(regs[a+1]);
      mrb_method_t m;

      MRB_METHOD_FROM_PROC(m, p);
      mrb_define_method_raw(mrb, target, syms[b], m);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_SCLASS, B) {
      regs[a] = mrb_singleton_class(mrb, regs[a]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_TCLASS, B) {
      if (!check_target_class(mrb)) goto L_RAISE;
      regs[a] = mrb_obj_value(mrb->c->ci->target_class);
      NEXT;
    }

    CASE(OP_ALIAS, BB) {
      struct RClass *target;

      if (!check_target_class(mrb)) goto L_RAISE;
      target = mrb->c->ci->target_class;
      mrb_alias_method(mrb, target, syms[a], syms[b]);
      NEXT;
    }
    CASE(OP_UNDEF, B) {
      struct RClass *target;

      if (!check_target_class(mrb)) goto L_RAISE;
      target = mrb->c->ci->target_class;
      mrb_undef_method_id(mrb, target, syms[a]);
      NEXT;
    }

    CASE(OP_DEBUG, Z) {
      FETCH_BBB();
#ifdef MRB_ENABLE_DEBUG_HOOK
      mrb->debug_op_hook(mrb, irep, pc, regs);
#else
#ifndef MRB_DISABLE_STDIO
      printf("OP_DEBUG %d %d %d\n", a, b, c);
#else
      abort();
#endif
#endif
      NEXT;
    }

    CASE(OP_ERR, B) {
      mrb_value msg = mrb_str_dup(mrb, pool[a]);
      mrb_value exc;

      exc = mrb_exc_new_str(mrb, E_LOCALJUMP_ERROR, msg);
      ERR_PC_SET(mrb);
      mrb_exc_set(mrb, exc);
      goto L_RAISE;
    }

    CASE(OP_EXT1, Z) {
      insn = READ_B();
      switch (insn) {
#define OPCODE(insn,ops) case OP_ ## insn: FETCH_ ## ops ## _1(); goto L_OP_ ## insn ## _BODY;
#include "mruby/ops.h"
#undef OPCODE
      }
      pc--;
      NEXT;
    }
    CASE(OP_EXT2, Z) {
      insn = READ_B();
      switch (insn) {
#define OPCODE(insn,ops) case OP_ ## insn: FETCH_ ## ops ## _2(); goto L_OP_ ## insn ## _BODY;
#include "mruby/ops.h"
#undef OPCODE
      }
      pc--;
      NEXT;
    }
    CASE(OP_EXT3, Z) {
      uint8_t insn = READ_B();
      switch (insn) {
#define OPCODE(insn,ops) case OP_ ## insn: FETCH_ ## ops ## _3(); goto L_OP_ ## insn ## _BODY;
#include "mruby/ops.h"
#undef OPCODE
      }
      pc--;
      NEXT;
    }

    CASE(OP_STOP, Z) {
      /*        stop VM */
    L_STOP:
      while (mrb->c->eidx > 0) {
        ecall(mrb);
      }
      mrb->c->cibase->ridx = 0;
      ERR_PC_CLR(mrb);
      mrb->jmp = prev_jmp;
      if (mrb->exc) {
        return mrb_obj_value(mrb->exc);
      }
      return regs[irep->nlocals];
    }
  }
  END_DISPATCH;
#undef regs
  }
  MRB_CATCH(&c_jmp) {
    exc_catched = TRUE;
    goto RETRY_TRY_BLOCK;
  }
  MRB_END_EXC(&c_jmp);
}

static mrb_value
mrb_run(mrb_state *mrb, struct RProc *proc, mrb_value self)
{
  if (mrb->c->ci->argc < 0) {
    return mrb_vm_run(mrb, proc, self, 3); /* receiver, args and block) */
  }
  else {
    return mrb_vm_run(mrb, proc, self, mrb->c->ci->argc + 2); /* argc + 2 (receiver and block) */
  }
}

MRB_API mrb_value
mrb_top_run(mrb_state *mrb, struct RProc *proc, mrb_value self, unsigned int stack_keep)
{
  mrb_callinfo *ci;
  mrb_value v;

  if (!mrb->c->cibase) {
    return mrb_vm_run(mrb, proc, self, stack_keep);
  }
  if (mrb->c->ci == mrb->c->cibase) {
    mrb->c->ci->env = NULL;
    return mrb_vm_run(mrb, proc, self, stack_keep);
  }
  ci = cipush(mrb);
  ci->stackent = mrb->c->stack;
  ci->mid = 0;
  ci->acc = CI_ACC_SKIP;
  ci->target_class = mrb->object_class;
  v = mrb_vm_run(mrb, proc, self, stack_keep);

  return v;
}

#if defined(MRB_ENABLE_CXX_EXCEPTION) && defined(__cplusplus)
# if !defined(MRB_ENABLE_CXX_ABI)
} /* end of extern "C" */
# endif
mrb_int mrb_jmpbuf::jmpbuf_id = 0;
# if !defined(MRB_ENABLE_CXX_ABI)
extern "C" {
# endif
#endif
