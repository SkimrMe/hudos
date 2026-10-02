/* tls_bn.h - minimal big-integer arithmetic for RSA (up to 2048-bit).
 * Little-endian limbs (limb 0 = least significant 32 bits).
 * No libc / no stdio: freestanding-kernel safe. */
#ifndef HUDOS_TLS_BN_H
#define HUDOS_TLS_BN_H

#include "tls.h"

/* Enough limbs to hold a 4096-bit RSA modulus (128 limbs) AND the product of
 * two such numbers inside bn_mul (max 256 limbs). Real public root CAs (e.g.
 * ISRG X1 / Let's Encrypt) are 4096-bit, so the accumulator must be >= 256.
 * 256 limbs = 8192 bits of headroom. */
#define BN_LIMBS 256

typedef struct { UINT32 d[BN_LIMBS]; int n; } bn;

void bn_zero(bn *a);
void bn_one(bn *a);
void bn_copy(bn *dst, const bn *src);
/* import/export big-endian bytes (DER-style) */
void bn_from_be(bn *a, const UINT8 *be, int len);
void bn_to_be(const bn *a, UINT8 *out, int len);
int  bn_cmp(const bn *a, const bn *b);   /* -1 <, 0 ==, 1 > */
int  bn_is_zero(const bn *a);
/* r = a + b */
void bn_add(bn *r, const bn *a, const bn *b);
/* r = a - b  (requires a >= b; returns -1 and leaves r unchanged if a < b) */
int  bn_sub(bn *r, const bn *a, const bn *b);
/* r = a * b  (r may alias neither; r->n may reach 2*min(a.n,b.n)) */
void bn_mul(bn *r, const bn *a, const bn *b);
/* q = u / v, r = u % v  (q or r may be NULL) */
void bn_divmod(bn *q, bn *r, const bn *u, const bn *v);
/* out = base^exp mod mod  (base is reduced mod mod first) */
void bn_modexp(bn *out, const bn *base, const bn *exp, const bn *mod);

#endif /* HUDOS_TLS_BN_H */
