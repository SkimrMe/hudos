/* tls.h - self-contained crypto primitives for a minimal TLS 1.2 client.
 * No libc dependency: works both in the freestanding hudos kernel (compiled
 * together with hudos_server.c) and on a hosted test harness. */
#ifndef HUDOS_TLS_H
#define HUDOS_TLS_H

/* Type aliases. Guarded so the same header can be (a) compiled standalone on
 * the host, and (b) text-#included into hudos_server.c, which already defines
 * UINT8..UINT64 via efi.h (uint8_t etc.). When the includer has already set
 * HUDOS_TLS_TYPES, skip these to avoid a redefinition. */
#ifndef HUDOS_TLS_TYPES
#define HUDOS_TLS_TYPES
typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned int    UINT32;
typedef unsigned long long UINT64;
typedef long long           INT64;
#endif

/* ---- SHA-256 ---- */
/* Incremental context for streaming hashes (e.g. the handshake transcript). */
typedef struct { UINT32 h[8]; UINT8 buf[64]; UINT32 bufn; UINT64 len; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const UINT8 *p, UINT32 n);
void sha256_final(sha256_ctx *c, UINT8 out[32]);
void sha256(const UINT8 *msg, UINT32 len, UINT8 out[32]);

/* ---- HMAC-SHA256 ---- */
void hmac_sha256(const UINT8 *key, UINT32 klen,
                 const UINT8 *msg, UINT32 mlen, UINT8 out[32]);

/* ---- ChaCha20 (RFC 8439): key 32B, nonce 12B, 32-bit counter ---- */
void chacha20(const UINT8 key[32], const UINT8 nonce[12], UINT32 counter,
              const UINT8 *in, UINT32 len, UINT8 *out);

/* ---- Poly1305 (RFC 8439): key 32B, msg arbitrary, 16B mac ---- */
void poly1305(const UINT8 *msg, UINT32 len, const UINT8 key[32], UINT8 mac[16]);

/* ---- ChaCha20-Poly1305 AEAD (RFC 8439) ---- */
/* Returns 0 on success (tag verified for decrypt). */
int aead_chacha20poly1305(int encrypt,            /* 1=encrypt, 0=decrypt */
                          const UINT8 key[32],
                          const UINT8 nonce[12],
                          const UINT8 *aad, UINT32 aadlen,
                          const UINT8 *in, UINT32 inlen,
                          UINT8 *out,             /* ciphertext/plaintext */
                          UINT8 tag[16]);

/* ---- P-256 ECDH ---- */
/* Generate an ephemeral key pair. priv is 32 random bytes (caller fills via
 * tls_rng). pub_x/pub_y are 32-byte big-endian outputs. Returns 0 on success. */
int p256_keypair(const UINT8 priv[32], UINT8 pub_x[32], UINT8 pub_y[32]);

/* Compute shared secret = priv * peer_pub (x-coordinate), 32-byte big-endian.
 * Returns 0 on success. peer_pub_{x,y} big-endian. */
int p256_ecdh(const UINT8 priv[32],
              const UINT8 peer_pub_x[32], const UINT8 peer_pub_y[32],
              UINT8 shared[32]);

/* ---- RNG hook ---- */
typedef int (*tls_rng_fn)(UINT8 *buf, int n);   /* returns n bytes, or <0 */
extern tls_rng_fn tls_rng;
void tls_set_rng(tls_rng_fn f);

/* ---- Self-test (RFC vectors). Returns 0 if all pass. ---- */
int tls_selftest(void);

#endif /* HUDOS_TLS_H */
