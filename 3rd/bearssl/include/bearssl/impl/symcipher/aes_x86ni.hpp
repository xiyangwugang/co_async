#ifndef BEARSSL_IMPL_SYMCIPHER_AES_X86NI_HPP
#define BEARSSL_IMPL_SYMCIPHER_AES_X86NI_HPP

/*
 * Copyright (c) 2017 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining 
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be 
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "inner.h"

/*
 * This code contains the AES key schedule implementation using the
 * AES-NI opcodes.
 */

#if BR_AES_X86NI

#if BR_AES_X86NI_GCC
#if BR_AES_X86NI_GCC_OLD
#pragma GCC push_options
#pragma GCC target("sse2,sse4.1,aes,pclmul")
#endif
#include <wmmintrin.h>
//#include <cpuid.h>
#if BR_AES_X86NI_GCC_OLD
#pragma GCC pop_options
#endif
#endif

#if BR_AES_X86NI_MSC
#include <intrin.h>
#endif

/* see inner.h */
 inline int
br_aes_x86ni_supported(void)
{
	/*
	 * Bit mask for features in ECX:
	 *   19   SSE4.1 (used for _mm_insert_epi32(), for AES-CTR)
	 *   25   AES-NI
	 */
#define _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK   0x02080000

#if BR_AES_X86NI_GCC
	unsigned eax, ebx, ecx, edx;

	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
		return (ecx & _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK) == _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK;
	} else {
		return 0;
	}
#elif BR_AES_X86NI_MSC
	int info[4];

	__cpuid(info, 1);
	return ((uint32_t)info[2] & _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK) == _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK;
#else
	return 0;
#endif

#undef _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_MASK
}

/*
 * Per-function attributes appear unreliable on old GCC, so we use the
 * pragma for all remaining functions in this file.
 */
#if BR_AES_X86NI_GCC_OLD
#pragma GCC target("sse2,sse4.1,aes,pclmul")
#endif

BR_TARGET("sse2,aes")
static inline __m128i
expand_step128(__m128i k, __m128i k2)
{
	k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
	k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
	k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
	k2 = _mm_shuffle_epi32(k2, 0xFF);
	return _mm_xor_si128(k, k2);
}

BR_TARGET("sse2,aes")
static inline void
expand_step192(__m128i *t1, __m128i *t2, __m128i *t3)
{
	__m128i t4;

	*t2 = _mm_shuffle_epi32(*t2, 0x55);
	t4 = _mm_slli_si128(*t1, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	*t1 = _mm_xor_si128(*t1, *t2);
	*t2 = _mm_shuffle_epi32(*t1, 0xFF);
	t4 = _mm_slli_si128(*t3, 0x4);
	*t3 = _mm_xor_si128(*t3, t4);
	*t3 = _mm_xor_si128(*t3, *t2);
}

BR_TARGET("sse2,aes")
static inline void
expand_step256_1(__m128i *t1, __m128i *t2)
{
	__m128i t4;

	*t2 = _mm_shuffle_epi32(*t2, 0xFF);
	t4 = _mm_slli_si128(*t1, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t1 = _mm_xor_si128(*t1, t4);
	*t1 = _mm_xor_si128(*t1, *t2);
}

BR_TARGET("sse2,aes")
static inline void
expand_step256_2(__m128i *t1, __m128i *t3)
{
	__m128i t2, t4;

	t4 = _mm_aeskeygenassist_si128(*t1, 0x0);
	t2 = _mm_shuffle_epi32(t4, 0xAA);
	t4 = _mm_slli_si128(*t3, 0x4);
	*t3 = _mm_xor_si128(*t3, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t3 = _mm_xor_si128(*t3, t4);
	t4 = _mm_slli_si128(t4, 0x4);
	*t3 = _mm_xor_si128(*t3, t4);
	*t3 = _mm_xor_si128(*t3, t2);
}

/*
 * Perform key schedule for AES, encryption direction. Subkeys are written
 * in sk[], and the number of rounds is returned. Key length MUST be 16,
 * 24 or 32 bytes.
 */
BR_TARGET("sse2,aes")
static unsigned
x86ni_keysched(__m128i *sk, const void *key, size_t len)
{
	const unsigned char *kb;

#define _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(k, i, rcon)   do { \
		k = expand_step128(k, _mm_aeskeygenassist_si128(k, rcon)); \
		sk[i] = k; \
	} while (0)

#define _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192(i, rcon1, rcon2)   do { \
		sk[(i) + 0] = t1; \
		sk[(i) + 1] = t3; \
		t2 = _mm_aeskeygenassist_si128(t3, rcon1); \
		expand_step192(&t1, &t2, &t3); \
		sk[(i) + 1] = _mm_castpd_si128(_mm_shuffle_pd( \
			_mm_castsi128_pd(sk[(i) + 1]), \
			_mm_castsi128_pd(t1), 0)); \
		sk[(i) + 2] = _mm_castpd_si128(_mm_shuffle_pd( \
			_mm_castsi128_pd(t1), \
			_mm_castsi128_pd(t3), 1)); \
		t2 = _mm_aeskeygenassist_si128(t3, rcon2); \
		expand_step192(&t1, &t2, &t3); \
	} while (0)

#define _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256(i, rcon)   do { \
		sk[(i) + 0] = t3; \
		t2 = _mm_aeskeygenassist_si128(t3, rcon); \
		expand_step256_1(&t1, &t2); \
		sk[(i) + 1] = t1; \
		expand_step256_2(&t1, &t3); \
	} while (0)

	kb = (unsigned char *)key;
	switch (len) {
		__m128i t1, t2, t3;

	case 16:
		t1 = _mm_loadu_si128((const __m128i *)kb);
		sk[0] = t1;
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  1, 0x01);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  2, 0x02);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  3, 0x04);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  4, 0x08);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  5, 0x10);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  6, 0x20);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  7, 0x40);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  8, 0x80);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1,  9, 0x1B);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128(t1, 10, 0x36);
		return 10;

	case 24:
		t1 = _mm_loadu_si128((const __m128i *)kb);
		t3 = _mm_loadu_si128((const __m128i *)(kb + 8));
		t3 = _mm_shuffle_epi32(t3, 0x4E);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192(0, 0x01, 0x02);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192(3, 0x04, 0x08);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192(6, 0x10, 0x20);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192(9, 0x40, 0x80);
		sk[12] = t1;
		return 12;

	case 32:
		t1 = _mm_loadu_si128((const __m128i *)kb);
		t3 = _mm_loadu_si128((const __m128i *)(kb + 16));
		sk[0] = t1;
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256( 1, 0x01);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256( 3, 0x02);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256( 5, 0x04);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256( 7, 0x08);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256( 9, 0x10);
		_opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256(11, 0x20);
		sk[13] = t3;
		t2 = _mm_aeskeygenassist_si128(t3, 0x40);
		expand_step256_1(&t1, &t2);
		sk[14] = t1;
		return 14;

	default:
		return 0;
	}

#undef _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP128
#undef _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP192
#undef _opt_libs_BearSSL_src_symcipher_aes_x86ni_c_KEXP256
}

/* see inner.h */
BR_TARGET("sse2,aes")
unsigned
br_aes_x86ni_keysched_enc(unsigned char *skni, const void *key, size_t len)
{
	__m128i sk[15];
	unsigned num_rounds;

	num_rounds = x86ni_keysched(sk, key, len);
	memcpy(skni, sk, (num_rounds + 1) << 4);
	return num_rounds;
}

/* see inner.h */
BR_TARGET("sse2,aes")
unsigned
br_aes_x86ni_keysched_dec(unsigned char *skni, const void *key, size_t len)
{
	__m128i sk[15];
	unsigned u, num_rounds;

	num_rounds = x86ni_keysched(sk, key, len);
	_mm_storeu_si128((__m128i *)skni, sk[num_rounds]);
	for (u = 1; u < num_rounds; u ++) {
		_mm_storeu_si128((__m128i *)(skni + (u << 4)),
			_mm_aesimc_si128(sk[num_rounds - u]));
	}
	_mm_storeu_si128((__m128i *)(skni + (num_rounds << 4)), sk[0]);
	return num_rounds;
}

#endif
#endif
