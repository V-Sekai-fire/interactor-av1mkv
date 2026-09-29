/*! @file mmx2neon.h

	The MMX half of the ARM port.

	sse2neon covers the 128-bit SSE/SSE2 surface, and nothing covers MMX. This codec
	still uses it heavily -- 1687 `__m64` declarations and roughly 4900 `_mm_*_pi*`
	calls -- so the 64-bit intrinsics are supplied here.

	The mapping is direct rather than emulated. MMX is 64 bits wide and so is a NEON
	D register, so `__m64` is a NEON 64-bit vector and every operation below is one
	instruction on the matching lane type. Saturating adds are `vqadd`, packs are
	`vqmovn`, unpacks are `vzip`. Nothing here widens to 128 bits and narrows back.

	WHAT IS NOT HERE, AND WHY: `_mm_max_pi16`, `_mm_movemask_pi8` and `_mm_empty`.
	sse2neon defines these despite them being MMX, so defining them again here is a
	redefinition rather than a fallback: `_mm_max_pi16`, `_mm_movemask_pi8`,
	`_mm_empty`, `_mm_malloc`, `_mm_free`, `_mm_extract_pi16`, `_mm_insert_pi16` and
	`_mm_shuffle_pi16`. Everything below is what sse2neon genuinely leaves out.

	(C) 2026 -- Apache-2.0 OR MIT, matching the workspace.
*/
#ifndef MMX2NEON_H
#define MMX2NEON_H

#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>
#include <stdint.h>

typedef int64x1_t __m64;

/* -- reinterpretation helpers ------------------------------------------- */
#define MM64_S8(x)  vreinterpret_s8_s64(x)
#define MM64_S16(x) vreinterpret_s16_s64(x)
#define MM64_S32(x) vreinterpret_s32_s64(x)
#define MM64_U8(x)  vreinterpret_u8_s64(x)
#define MM64_U16(x) vreinterpret_u16_s64(x)
#define MM64_U32(x) vreinterpret_u32_s64(x)
#define MM64_FROM(x) vreinterpret_s64_##x

/* -- set / zero ---------------------------------------------------------- */
static inline __m64 _mm_setzero_si64(void) { return vdup_n_s64(0); }
static inline __m64 _mm_set1_pi8(char v) { return vreinterpret_s64_s8(vdup_n_s8((int8_t)v)); }
static inline __m64 _mm_set1_pi16(short v) { return vreinterpret_s64_s16(vdup_n_s16((int16_t)v)); }
static inline __m64 _mm_set1_pi32(int v) { return vreinterpret_s64_s32(vdup_n_s32((int32_t)v)); }
static inline __m64 _mm_cvtsi32_si64(int v) { return vreinterpret_s64_s32(vset_lane_s32((int32_t)v, vdup_n_s32(0), 0)); }

static inline __m64 _mm_set_pi16(short e3, short e2, short e1, short e0) {
	int16_t d[4] = { (int16_t)e0, (int16_t)e1, (int16_t)e2, (int16_t)e3 };
	return vreinterpret_s64_s16(vld1_s16(d));
}
static inline __m64 _mm_set_pi8(char e7, char e6, char e5, char e4, char e3, char e2, char e1, char e0) {
	int8_t d[8] = { (int8_t)e0, (int8_t)e1, (int8_t)e2, (int8_t)e3,
					(int8_t)e4, (int8_t)e5, (int8_t)e6, (int8_t)e7 };
	return vreinterpret_s64_s8(vld1_s8(d));
}
static inline __m64 _mm_setr_pi8(char e0, char e1, char e2, char e3, char e4, char e5, char e6, char e7) {
	return _mm_set_pi8(e7, e6, e5, e4, e3, e2, e1, e0);
}

/* -- load / store -------------------------------------------------------- */
static inline __m64 _mm_load_si64(const __m64 *p) { return vld1_s64((const int64_t *)p); }
static inline void _mm_store_si64(__m64 *p, __m64 a) { vst1_s64((int64_t *)p, a); }

/* -- bitwise ------------------------------------------------------------- */
static inline __m64 _mm_and_si64(__m64 a, __m64 b) { return vand_s64(a, b); }
static inline __m64 _mm_or_si64(__m64 a, __m64 b) { return vorr_s64(a, b); }
static inline __m64 _mm_xor_si64(__m64 a, __m64 b) { return veor_s64(a, b); }
static inline __m64 _mm_andnot_si64(__m64 a, __m64 b) { return vbic_s64(b, a); } /* ~a & b */

/* -- add / subtract, wrapping and saturating ------------------------------ */
static inline __m64 _mm_add_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vadd_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_add_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vadd_s32(MM64_S32(a), MM64_S32(b))); }
static inline __m64 _mm_sub_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vsub_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_sub_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vsub_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_sub_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vsub_s32(MM64_S32(a), MM64_S32(b))); }

static inline __m64 _mm_adds_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vqadd_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_adds_pu8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vqadd_u8(MM64_U8(a), MM64_U8(b))); }
static inline __m64 _mm_adds_pu16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vqadd_u16(MM64_U16(a), MM64_U16(b))); }
static inline __m64 _mm_subs_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vqsub_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_subs_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vqsub_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_subs_pu8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vqsub_u8(MM64_U8(a), MM64_U8(b))); }
static inline __m64 _mm_subs_pu16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vqsub_u16(MM64_U16(a), MM64_U16(b))); }

/* -- multiply ------------------------------------------------------------ */
static inline __m64 _mm_mullo_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vmul_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_mulhi_pi16(__m64 a, __m64 b) {
	/* Widen, multiply, keep the top half -- NEON has no 16x16->high-16 in one step. */
	int32x4_t p = vmull_s16(MM64_S16(a), MM64_S16(b));
	return vreinterpret_s64_s16(vshrn_n_s32(p, 16));
}

/* -- compare ------------------------------------------------------------- */
static inline __m64 _mm_cmpeq_pi8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vceq_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_cmpeq_pi16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vceq_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_cmpgt_pi8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vcgt_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_cmpgt_pi16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vcgt_s16(MM64_S16(a), MM64_S16(b))); }

/* -- shifts. The immediate forms must stay macros: NEON shift counts are
	  compile-time operands, and a function parameter is not one. ---------- */
#define _mm_slli_pi16(a, n) vreinterpret_s64_s16(vshl_n_s16(MM64_S16(a), (n)))
#define _mm_slli_pi32(a, n) vreinterpret_s64_s32(vshl_n_s32(MM64_S32(a), (n)))
#define _mm_slli_si64(a, n) vshl_n_s64((a), (n))
#define _mm_srli_pi16(a, n) vreinterpret_s64_u16(vshr_n_u16(MM64_U16(a), (n)))
#define _mm_srli_pi32(a, n) vreinterpret_s64_u32(vshr_n_u32(MM64_U32(a), (n)))
#define _mm_srli_si64(a, n) vreinterpret_s64_u64(vshr_n_u64(vreinterpret_u64_s64(a), (n)))
#define _mm_srai_pi16(a, n) vreinterpret_s64_s16(vshr_n_s16(MM64_S16(a), (n)))
#define _mm_srai_pi32(a, n) vreinterpret_s64_s32(vshr_n_s32(MM64_S32(a), (n)))

/* Variable shift: the count arrives in a register, so negate and use vshl. */
static inline __m64 _mm_sra_pi16(__m64 a, __m64 count) {
	int16_t c = (int16_t)vget_lane_s64(count, 0);
	return vreinterpret_s64_s16(vshl_s16(MM64_S16(a), vdup_n_s16((int16_t)-c)));
}

/* -- pack, with saturation ------------------------------------------------ */
static inline __m64 _mm_packs_pi16(__m64 a, __m64 b) {
	int16x8_t j = vcombine_s16(MM64_S16(a), MM64_S16(b));
	return vreinterpret_s64_s8(vqmovn_s16(j));
}
static inline __m64 _mm_packs_pi32(__m64 a, __m64 b) {
	int32x4_t j = vcombine_s32(MM64_S32(a), MM64_S32(b));
	return vreinterpret_s64_s16(vqmovn_s32(j));
}
static inline __m64 _mm_packs_pu16(__m64 a, __m64 b) {
	int16x8_t j = vcombine_s16(MM64_S16(a), MM64_S16(b));
	return vreinterpret_s64_u8(vqmovun_s16(j));
}

/* -- interleave ----------------------------------------------------------- */
static inline __m64 _mm_unpacklo_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vzip1_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_unpackhi_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vzip2_s8(MM64_S8(a), MM64_S8(b))); }
static inline __m64 _mm_unpacklo_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vzip1_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_unpackhi_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s16(vzip2_s16(MM64_S16(a), MM64_S16(b))); }
static inline __m64 _mm_unpacklo_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vzip1_s32(MM64_S32(a), MM64_S32(b))); }
static inline __m64 _mm_unpackhi_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vzip2_s32(MM64_S32(a), MM64_S32(b))); }

/* -- lane access and movemask --------------------------------------------- */




#endif /* __aarch64__ || __ARM_NEON */
#endif /* MMX2NEON_H */
