/*! @file simd_compat.h

	One include in place of <emmintrin.h>.

	On x86 this is <emmintrin.h> and nothing else, so that target is bit-for-bit
	what it was. On ARM it is sse2neon for the 128-bit surface, mmx2neon.h for the
	64-bit one, and a posix_memalign pair for the two allocator intrinsics that
	live in the same Intel header and are not vector operations at all.

	WHY A HEADER RATHER THAN A BUILD FLAG. The includes were unconditional in 27
	files. Guarding each one in place would put the same seven preprocessor lines
	in 27 places, and the next file added would be written from the pattern next
	to it -- unconditional again. One header is one place to be wrong.

	(C) 2026 -- Apache-2.0 OR MIT, matching the workspace.
*/
#ifndef SIMD_COMPAT_H
#define SIMD_COMPAT_H

#if defined(__aarch64__) || defined(__ARM_NEON)

/* sse2neon reads the SSE level from these; the codec targets SSE2. */
#ifndef SSE2NEON_SUPPRESS_WARNINGS
#define SSE2NEON_SUPPRESS_WARNINGS 1
#endif
#include "sse2neon.h"
#include "mmx2neon.h"

#include <stdlib.h>

/* sse2neon supplies _mm_malloc and _mm_free as well, so nothing is added here.
   They are allocator calls rather than instructions, which is why they are easy
   to miss when auditing what a SIMD shim covers. */

#else /* x86: unchanged */

/* NOT simd_compat.h. The bulk rewrite that replaced <emmintrin.h> across the tree
   also rewrote this line, and the include guard made the self-include a silent
   no-op -- so x86 got no intrinsics at all and every __m128i became undeclared.
   Compiling on ARM cannot catch that; only building for x86 can. */
#include <emmintrin.h>

/* The aligned allocators live in their own Intel header, which GCC needs named
   explicitly. Apple's clang folds them in elsewhere and has no such file, which is
   why the codec guarded this with __APPLE__ rather than by architecture -- and why
   an arm64 Linux builder was the first thing to fail on it. */
#ifndef __APPLE__
#include <mm_malloc.h>
#endif

#endif

#endif /* SIMD_COMPAT_H */
