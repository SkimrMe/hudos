/* x509.c - minimal X.509 (DER) parser. Built on tls_rsa.c. */
#include "tls.h"
#include "tls_rsa.h"
#include "x509.h"

static int bmemcmp(const UINT8 *a, const UINT8 *b, int n){
    for(int i=0;i<n;i++) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return 0;
}

/* ---- generic DER TLV cursor ----
 * Parses one TLV at *pp (with *plen remaining). On success sets *tag,
 * *out = content pointer, *outlen = content length, and advances the cursor. */
static int der_next(const UINT8 **pp, int *plen, int *tag,
                   const UINT8 **out, int *outlen){
    const UINT8 *p = *pp; int rem = *plen;
    if(rem < 2) return -1;
    int t = p[0]; int idx = 1;
    int l = p[1];
    int consumed_len;
    if(l < 0x80){ consumed_len = l; idx = 2; }
    else if(l == 0x80){ return -1; }              /* indefinite: unsupported */
    else {
        int nb = l & 0x7f;
        if(nb > 4) return -1;                     /* length too large */
        if(rem < 2+nb) return -1;
        consumed_len = 0;
        for(int i=0;i<nb;i++) consumed_len = (consumed_len<<8)|p[2+i];
        idx = 2+nb;
    }
    if(consumed_len > rem - idx) return -1;
    *tag = t;
    *out = p + idx;
    *outlen = consumed_len;
    *pp = p + idx + consumed_len;
    *plen = rem - (idx + consumed_len);
    return 0;
}

/* rsaEncryption OID 1.2.840.113549.1.1.1 */
static const UINT8 RSA_OID[9] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
/* sha256WithRSAEncryption OID 1.2.840.113549.1.1.11 */
static const UINT8 SHA256RSA_OID[9] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};

/* Extract RSA (n,e) from a subjectPublicKeyInfo BIT STRING's key payload.
 * key points just past the BIT STRING's 1 unused-bits byte, and is a DER
 * SEQUENCE { INTEGER n, INTEGER e }. */
static int parse_rsa_pubkey(const UINT8 *key, int keylen,
                            const UINT8 **n, int *nlen,
                            const UINT8 **e, int *elen){
    const UINT8 *kp = key; int krem = keylen;
    const UINT8 *seq; int sl, st;
    if(der_next(&kp,&krem,&st,&seq,&sl)!=0 || st!=0x30) return -1; /* SEQUENCE */
    /* parse INTEGERs from inside the SEQUENCE content (seq), not kp */
    const UINT8 *sp = seq; int srem = sl;
    const UINT8 *nn; int nl, nt;
    if(der_next(&sp,&srem,&nt,&nn,&nl)!=0 || nt!=0x02) return -1;  /* INTEGER n */
    const UINT8 *ee; int el, et;
    if(der_next(&sp,&srem,&et,&ee,&el)!=0 || et!=0x02) return -1;  /* INTEGER e */
    *n = nn; *nlen = nl; *e = ee; *elen = el;
    return 0;
}

int x509_parse(const UINT8 *der, int len, x509_cert *c){
    const UINT8 *p = der; int rem = len;
    int tag; const UINT8 *body; int blen;
    if(der_next(&p,&rem,&tag,&body,&blen)!=0 || tag!=0x30) return -1; /* Certificate SEQ */

    const UINT8 *cp = body; int crem = blen;
    const UINT8 *tbs_start = cp;            /* TBSCertificate TLV start (incl. tag+len) */
    const UINT8 *tbs; int tbslen;
    if(der_next(&cp,&crem,&tag,&tbs,&tbslen)!=0 || tag!=0x30) return -1; /* tbsCertificate */
    c->tbs = tbs_start; c->tbs_len = (int)(cp - tbs_start);  /* full TLV is what's signed */

    /* signatureAlgorithm SEQUENCE, then signatureValue BIT STRING */
    const UINT8 *salg; int salglen;
    if(der_next(&cp,&crem,&tag,&salg,&salglen)!=0 || tag!=0x30) return -1;
    {   /* capture the algorithm OID for later policy checks */
        const UINT8 *sp=salg; int srem=salglen;
        const UINT8 *oid; int ol, ot;
        if(der_next(&sp,&srem,&ot,&oid,&ol)!=0 || ot!=0x06) return -1;
        c->salg = oid; c->salg_len = ol;
    }
    const UINT8 *sig; int siglen;
    if(der_next(&cp,&crem,&tag,&sig,&siglen)!=0 || tag!=0x03) return -1; /* BIT STRING */
    if(siglen < 1) return -1;
    c->sig = sig+1; c->sig_len = siglen-1;

    /* Find the subjectPublicKeyInfo by walking tbs fields and matching the
     * rsaEncryption OID. */
    const UINT8 *tp = tbs; int trem = tbslen;
    int found = 0;
    while(trem > 0){
        const UINT8 *field; int flen, ftag;
        if(der_next(&tp,&trem,&ftag,&field,&flen)!=0) break;
        if(ftag != 0x30) continue;                 /* only consider SEQUENCEs */
        const UINT8 *sp = field; int srem = flen;
        const UINT8 *algid; int alen, atag;
        if(der_next(&sp,&srem,&atag,&algid,&alen)!=0 || atag!=0x30) continue;
        const UINT8 *oid; int ol, ot;
        if(der_next(&algid,&alen,&ot,&oid,&ol)!=0 || ot!=0x06) continue;
        if(ol==9 && bmemcmp(oid, RSA_OID, 9)==0){
            /* this SEQUENCE is the spki. Its 2nd child is the BIT STRING. */
            const UINT8 *bs; int bsl, btag;
            if(der_next(&sp,&srem,&btag,&bs,&bsl)!=0 || btag!=0x03) return -1;
            if(bsl < 1) return -1;
            const UINT8 *key = bs+1; int keylen = bsl-1;
            if(parse_rsa_pubkey(key, keylen, &c->n, &c->n_len, &c->e, &c->e_len)!=0) return -1;
            /* strip a leading 0x00 sign byte so n_len/e_len are the true sizes */
            while(c->n_len>1 && c->n[0]==0x00){ c->n++; c->n_len--; }
            while(c->e_len>1 && c->e[0]==0x00){ c->e++; c->e_len--; }
            found = 1;
            break;
        }
    }
    if(!found) return -1;
    return 0;
}

int x509_verify_sig(const x509_cert *subject, const x509_cert *issuer){
    /* require RSA-SHA256 */
    if(subject->salg_len!=9 || bmemcmp(subject->salg, SHA256RSA_OID, 9)!=0) return -1;
    UINT8 digest[32];
    sha256(subject->tbs, (UINT32)subject->tbs_len, digest);
    return rsa_pkcs1_v15_verify_sha256(issuer->n, issuer->n_len,
                                       issuer->e, issuer->e_len,
                                       subject->sig, subject->sig_len, digest);
}

int x509_verify_chain(const x509_cert *certs, int count,
                      const UINT8 *pin_n, int pin_nlen,
                      const UINT8 *pin_e, int pin_elen){
    if(count < 1) return -1;
    /* Synthetic issuer holding only the pinned root public key. It is used to
     * verify the TOP received cert: that cert must be signed by this root
     * (e.g. leaf<-intermediate<-DigiCert-root). x509_verify_sig only reads the
     * issuer's (n,e), so this is sufficient. */
    x509_cert root;
    root.n = pin_n; root.n_len = pin_nlen;
    root.e = pin_e; root.e_len = pin_elen;
    for(int i=0;i<count;i++){
        const x509_cert *iss = (i+1 < count) ? &certs[i+1] : &root;
        if(x509_verify_sig(&certs[i], iss) != 0) return -1;
    }
    return 0;
}
