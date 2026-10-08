/* tls_handshake.h - TLS 1.2 full handshake state machine (client side).
 *
 * Builds on:
 *   tls.h      (SHA-256/HMAC/ChaCha20/Poly1305/AEAD/P-256/RNG)
 *   tls_cli.h  (PRF, record layer, key schedule, ClientHello)
 *   tls_rsa.h  (RSA PKCS#1 v1.5 verify)
 *   x509.h     (certificate parse + chain verify)
 *
 * Self-contained, NO libc / NO stdio -> freestanding-kernel safe.
 * All byte I/O is abstracted through tls_conn (send/recv callbacks) so the
 * exact same code runs (a) in the kernel over nw_tcp_*, and (b) on the host
 * over a POSIX socket for verification.
 *
 * Supported cipher suite (only one):
 *   TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 (0xCCA8)
 */
#ifndef HUDOS_TLS_HANDSHAKE_H
#define HUDOS_TLS_HANDSHAKE_H

#include "tls.h"
#include "tls_cli.h"

/* ---- I/O abstraction ---- */
typedef struct {
    /* send exactly len bytes; return len on success, <0 on error */
    int (*send)(void *ctx, const UINT8 *data, int len);
    /* receive up to max bytes; return 0..max (0 = EOF), <0 on error.
     * Caller loops until it has what it needs. */
    int (*recv)(void *ctx, UINT8 *buf, int max);
    void *ctx;
} tls_conn;

/* Pinned trust store: one or more root CA certificates in DER form. A received
 * chain is accepted if ANY anchor verifies it (standard trust-anchor model).
 * n must be in [1, TLS_TRUST_MAX]. */
#define TLS_TRUST_MAX 16
typedef struct {
    int n;
    const UINT8 *certs[TLS_TRUST_MAX];
    int         lens[TLS_TRUST_MAX];
} tls_trust;

/* Optional debug trace hook (host verification only). In the kernel build
 * this stays NULL and is never dereferenced, so it costs nothing and keeps
 * the file freestanding-safe. */
typedef void (*tls_dbg_fn)(const char *msg);
extern tls_dbg_fn tls_dbg;

typedef struct {
    tls_conn conn;
    UINT8 client_write_key[32];
    UINT8 server_write_key[32];
    UINT8 client_write_iv[12];    /* RFC 7905: fixed_iv_length = 12 bytes */
    UINT8 server_write_iv[12];
    UINT64 cli_seq;               /* client record sequence number */
    UINT64 srv_seq;               /* server record sequence number */
    int established;
    sha256_ctx trans;             /* running handshake transcript hash */
    UINT8 hsbuf[16384];           /* handshake reassembly buffer */
    int hspos;
    int hslen;
    UINT8 client_random[32];
    UINT8 server_random[32];
    UINT8 master[48];
} tls_session;

/* Perform a full TLS 1.2 handshake (cipher 0xCCA8) over an already-open TCP
 * connection. On success returns 0 and fills session keys; the connection
 * remains open for tls_send/tls_recv application data. */
int  tls_handshake(tls_session *s, const char *hostname, const tls_trust *trust);

/* Send/receive one TLS application_data record. tls_recv returns the plaintext
 * length, or <0 on error / close. */
int  tls_send(tls_session *s, const UINT8 *data, int len);
int  tls_recv(tls_session *s, UINT8 *buf, int max);

void tls_close(tls_session *s);

#endif /* HUDOS_TLS_HANDSHAKE_H */
