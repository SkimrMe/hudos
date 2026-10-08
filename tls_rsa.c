/* tls_rsa.c - RSA PKCS#1 v1.5 verification (SHA-256). Built on tls_bn.c. */
#include "tls_bn.h"
#include "tls_rsa.h"

static int bmemcmp(const UINT8 *a, const UINT8 *b, int n){
    for(int i=0;i<n;i++) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return 0;
}

int rsa_pkcs1_v15_verify_sha256(const UINT8 *mod_be, int mlen,
                                const UINT8 *exp_be, int elen,
                                const UINT8 *sig_be, int siglen,
                                const UINT8 digest[32]){
    if(mlen<=0 || siglen<=0 || elen<=0) return -1;

    bn N, E, S;
    bn_from_be(&N, mod_be, mlen);
    bn_from_be(&E, exp_be, elen);
    bn_from_be(&S, sig_be, siglen);
    if(bn_cmp(&S,&N)>=0) return -1;             /* signature must be < N */

    bn_modexp(&S, &S, &E, &N);                 /* S = sig^e mod N = EMSA block */

    UINT8 em[512];   /* up to 4096-bit modulus */
    if(mlen > (int)sizeof(em)) return -1;
    bn_to_be(&S, em, mlen);

    /* EMSA-PKCS1-v1_5: 0x00 || 0x01 || PS(0xFF) || 0x00 || T
     * T = DigestInfo(SHA-256) || H */
    static const UINT8 di[19]={
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
        0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    const int tlen = 19 + 32; /* 51 */

    if(em[0]!=0x00 || em[1]!=0x01) return -1;
    int idx=2;
    while(idx < mlen && em[idx]==0xff) idx++;
    if(idx>=mlen) return -1;
    if(em[idx]!=0x00) return -1;
    idx++;
    if(idx + tlen != mlen) return -1;          /* exact length check */
    if(bmemcmp(em+idx, di, 19)!=0) return -1;
    if(bmemcmp(em+idx+19, digest, 32)!=0) return -1;
    return 0;
}
