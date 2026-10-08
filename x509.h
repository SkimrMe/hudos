/* x509.h - minimal X.509 (DER) parser for TLS certificate validation.
 * Extracts the RSA public key from a Certificate, captures the exact
 * TBSCertificate bytes (for hashing) and the signatureValue, and verifies
 * a cert chain against a pinned top-certificate public key.
 * No libc / no stdio (freestanding-kernel safe). */
#ifndef HUDOS_X509_H
#define HUDOS_X509_H

#include "tls.h"

typedef struct {
    const UINT8 *tbs; int tbs_len;     /* exact TBSCertificate DER (signed data) */
    const UINT8 *sig; int sig_len;      /* signatureValue BIT STRING payload (big-endian) */
    const UINT8 *n;   int n_len;       /* RSA modulus (big-endian) */
    const UINT8 *e;   int e_len;       /* RSA public exponent (big-endian) */
    const UINT8 *salg; int salg_len;   /* signatureAlgorithm OID bytes */
} x509_cert;

/* Parse one DER-encoded Certificate. Returns 0 on success, -1 on error. */
int x509_parse(const UINT8 *der, int len, x509_cert *c);

/* Verify that 'subject' is signed by 'issuer' (RSA-SHA256).
 * Returns 0 if valid, -1 otherwise. */
int x509_verify_sig(const x509_cert *subject, const x509_cert *issuer);

/* Verify a chain certs[0..count-1] (leaf first). Each link i is signed by
 * certs[i+1]; the TOP cert (certs[count-1]) is verified against the pinned
 * root public key. Returns 0 if the whole chain is valid, -1 otherwise. */
int x509_verify_chain(const x509_cert *certs, int count,
                      const UINT8 *pin_n, int pin_nlen,
                      const UINT8 *pin_e, int pin_elen);

#endif /* HUDOS_X509_H */
