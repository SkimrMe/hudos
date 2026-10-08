/* tls_rsa.h - RSA PKCS#1 v1.5 signature verification (SHA-256).
 * Used to verify X.509 certificate signatures and TLS ServerKeyExchange
 * signatures. No libc / no stdio. */
#ifndef HUDOS_TLS_RSA_H
#define HUDOS_TLS_RSA_H

#include "tls.h"

/* Verify an RSA PKCS#1 v1.5 signature over SHA-256 digest.
 * mod_be/exp_be/sig_be are big-endian byte strings (lengths mlen/elen/siglen).
 * digest is the 32-byte SHA-256 of the signed data.
 * Returns 0 if the signature is valid, -1 otherwise. */
int rsa_pkcs1_v15_verify_sha256(const UINT8 *mod_be, int mlen,
                                const UINT8 *exp_be, int elen,
                                const UINT8 *sig_be, int siglen,
                                const UINT8 digest[32]);

#endif /* HUDOS_TLS_RSA_H */
