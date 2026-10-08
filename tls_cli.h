/* tls_cli.h - TLS 1.2 client handshake glue built on tls.h primitives.
 * Self-contained, no libc; usable both in the freestanding kernel and the
 * hosted test harness. The actual byte I/O (send/recv over the TCP stack) is
 * supplied by the caller via the tls_conn callbacks. */
#ifndef HUDOS_TLS_CLI_H
#define HUDOS_TLS_CLI_H

#include "tls.h"

/* ---- TLS 1.2 PRF (RFC 5246 §5), SHA-256 only ---- */
void tls_prf_sha256(const UINT8 *secret, int slen, const char *label,
                    const UINT8 *seed, int seedlen, UINT8 *out, int outlen);

/* ---- Records (ChaCha20-Poly1305, RFC 7905 12-byte nonce) ----
 * nonce = (00 00 00 00 || seq(8 bytes, big-endian)) XOR write_IV(12).
 * Caller maintains the 64-bit sequence number (per direction). Returns 0
 * on success; decrypt returns -1 on tag mismatch. */
/* Encrypt one TLS application_data / handshake record.
 * out must hold 5 (header) + len + 16 (tag). iv = 12-byte write_IV. */
int tls_record_encrypt(const UINT8 key[32], const UINT8 iv[12], UINT64 seq,
                       UINT8 content_type, const UINT8 *plain, int len,
                       UINT8 *out, int *out_len);
/* Decrypt a received record (header already stripped: ct is len+16).
 * plain must hold len bytes. Returns 0 on success. iv = 12-byte write_IV. */
int tls_record_decrypt(const UINT8 key[32], const UINT8 iv[12], UINT64 seq,
                       UINT8 content_type, const UINT8 *aad5,
                       const UINT8 *ct, int len, UINT8 *plain);

/* ---- Key schedule (ECDHE, ChaCha20-Poly1305) ----
 * pre_master = 32-byte big-endian ECDHE shared secret (x-coordinate).
 * client_random / server_random are 32 bytes each.
 * For ChaCha20-Poly1305 (RFC 7905) the key block is 88 bytes:
 *   client_write_key(32) || server_write_key(32) ||
 *   client_write_IV(12)  || server_write_IV(12).
 * Fills client_write_key[32] and server_write_key[32]. Returns 0.
 * (IVs are derived separately by the handshake code's own 88-byte split.) */
int tls_derive_keys(const UINT8 pre_master[32],
                    const UINT8 client_random[32],
                    const UINT8 server_random[32],
                    UINT8 client_write_key[32],
                    UINT8 server_write_key[32]);

/* ---- ClientHello ----
 * Builds a ClientHello for TLS 1.2 with cipher suite
 * TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 (0xCCA8), SNI = hostname,
 * supported_groups(secp256r1), ec_point_formats(uncompressed), and
 * signature_algorithms. Writes into out (caller-allocated, >=512).
 * Returns the total length, or -1 on error. */
int tls_build_client_hello(const char *hostname,
                           const UINT8 client_random[32],
                           UINT8 *out, int out_cap);

/* Host selftest for the above (returns 0 if all pass). */
int tls_cli_selftest(void);

#endif /* HUDOS_TLS_CLI_H */
