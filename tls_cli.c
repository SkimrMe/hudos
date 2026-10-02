/* tls_cli.c - TLS 1.2 client handshake glue (RFC 5246 / RFC 7905 / RFC 8439).
 * Built on tls.h primitives. NO libc / NO stdio (freestanding-kernel safe). */
#include "tls_cli.h"

/* local copies (tls.c keeps these private) */
static void mcp(UINT8 *d, const UINT8 *s, int n){ for(int i=0;i<n;i++) d[i]=s[i]; }
static void mset(UINT8 *d, UINT8 v, int n){ for(int i=0;i<n;i++) d[i]=v; }

/* ---------------- TLS 1.2 PRF (RFC 5246 §5), SHA-256 ---------------- */
/* P_hash(secret, seed) = HMAC(secret, A(1)||seed) | HMAC(secret, A(2)||seed) | ...
 * A(0)=seed, A(i)=HMAC(secret, A(i-1)). */
static void tls_phash(const UINT8 *secret, int slen,
                     const UINT8 *seed, int seedlen,
                     UINT8 *out, int outlen){
    UINT8 A[32];
    hmac_sha256(secret, (UINT32)slen, seed, (UINT32)seedlen, A); /* A(1) */
    int off = 0;
    while(off < outlen){
        /* blk holds A(32) || seed(seedlen); seed = label||rand can be ~96B,
         * so size generously to avoid overrunning the stack buffer. */
        UINT8 blk[256];
        int blen = 32 + seedlen;
        mcp(blk, A, 32);
        mcp(blk+32, seed, seedlen);
        UINT8 dig[32];
        hmac_sha256(secret, (UINT32)slen, blk, (UINT32)blen, dig);
        int n = outlen - off; if(n>32) n=32;
        mcp(out+off, dig, n);
        off += n;
        if(off < outlen) hmac_sha256(secret, (UINT32)slen, A, 32, A); /* next A */
    }
}

void tls_prf_sha256(const UINT8 *secret, int slen, const char *label,
                    const UINT8 *seed, int seedlen, UINT8 *out, int outlen){
    UINT8 ls[256]; int L=0;
    while(label[L]){ ls[L]=(UINT8)label[L]; L++; }
    mcp(ls+L, seed, seedlen);
    tls_phash(secret, slen, ls, L+seedlen, out, outlen);
}

/* ---------------- Records: ChaCha20-Poly1305 (RFC 7905) ----------------
 * TLS 1.2 AEAD nonce (12 bytes) = (00 00 00 00 || seq(8, big-endian)) XOR
 * write_IV(12).  RFC 7905 sets fixed_iv_length = 12 bytes.  A wrong nonce
 * makes every Poly1305 tag mismatch against a real server (record layer
 * failure). */
static void tls_seq_nonce(const UINT8 iv[12], UINT64 seq, UINT8 nonce[12]){
    /* top 4 bytes of the padded seq are zero, so they pass IV[0..3] through */
    nonce[0]=iv[0]; nonce[1]=iv[1]; nonce[2]=iv[2]; nonce[3]=iv[3];
    UINT8 s[8];
    for(int i=0;i<8;i++) s[i] = (UINT8)(seq >> (8*(7-i)));
    for(int i=0;i<8;i++) nonce[4+i] = (UINT8)(s[i] ^ iv[4+i]);
}

int tls_record_encrypt(const UINT8 key[32], const UINT8 iv[12], UINT64 seq,
                       UINT8 content_type, const UINT8 *plain, int len,
                       UINT8 *out, int *out_len){
    out[0] = content_type;
    out[1] = 0x03; out[2] = 0x03;
    /* Record header length = ciphertext + AEAD tag (plaintext + 16). */
    int fraglen = len + 16;
    out[3] = (UINT8)((fraglen>>8)&0xff); out[4] = (UINT8)(fraglen&0xff);
    UINT8 nonce[12]; tls_seq_nonce(iv, seq, nonce);
    /* RFC 5246 6.2.3.3 AAD = seq_num(8) || type(1) || version(2) ||
     * length(2).  The AAD length is the PLAINTEXT length (len), NOT len+16. */
    UINT8 aad[13];
    for(int i=0;i<8;i++) aad[i] = (UINT8)(seq >> (8*(7-i)));
    aad[8]=content_type; aad[9]=0x03; aad[10]=0x03;
    aad[11]=(UINT8)((len>>8)&0xff); aad[12]=(UINT8)(len&0xff);
    UINT8 tag[16];
    aead_chacha20poly1305(1, key, nonce, aad, 13, plain, (UINT32)len, out+5, tag);
    mcp(out+5+len, tag, 16);
    *out_len = 5 + len + 16;
    return 0;
}

int tls_record_decrypt(const UINT8 key[32], const UINT8 iv[12], UINT64 seq,
                       UINT8 content_type, const UINT8 *aad5,
                       const UINT8 *ct, int len, UINT8 *plain){
    (void)content_type;
    (void)aad5;
    UINT8 nonce[12]; tls_seq_nonce(iv, seq, nonce);
    /* AAD: seq(8) || header-type/version(3) || plaintext length(2).
     * The header's own length field (len+16) is NOT used here; the AAD
     * length must be the plaintext length `len`. */
    UINT8 aad[13];
    for(int i=0;i<8;i++) aad[i] = (UINT8)(seq >> (8*(7-i)));
    aad[8]=aad5[0]; aad[9]=aad5[1]; aad[10]=aad5[2];
    aad[11]=(UINT8)((len>>8)&0xff); aad[12]=(UINT8)(len&0xff);
    UINT8 tag[16]; mcp(tag, ct+len, 16);
    int r = aead_chacha20poly1305(0, key, nonce, aad, 13, ct, (UINT32)len, plain, tag);
    return r; /* 0 ok, -1 tag mismatch */
}

/* ---------------- Key schedule (ECDHE, ChaCha20-Poly1305) ----------------
 * For ChaCha20-Poly1305 (RFC 7905) the key block is 88 bytes:
 *   client_write_key(32) || server_write_key(32) ||
 *   client_write_IV(12)  || server_write_IV(12). */
int tls_derive_keys(const UINT8 pre_master[32],
                    const UINT8 client_random[32],
                    const UINT8 server_random[32],
                    UINT8 client_write_key[32],
                    UINT8 server_write_key[32]){
    UINT8 master[48];
    UINT8 seed[64]; mcp(seed, client_random, 32); mcp(seed+32, server_random, 32);
    tls_prf_sha256(pre_master, 32, "master secret", seed, 64, master, 48);

    UINT8 kbseed[64]; mcp(kbseed, server_random, 32); mcp(kbseed+32, client_random, 32);
    UINT8 keyblock[72];
    tls_prf_sha256(master, 48, "key expansion", kbseed, 64, keyblock, 72);
    mcp(client_write_key, keyblock, 32);
    mcp(server_write_key, keyblock+32, 32);
    return 0;
}

/* ---------------- ClientHello ---------------- */
static void put_u16(UINT8 *p, UINT16 v){ p[0]=(UINT8)(v>>8); p[1]=(UINT8)v; }

int tls_build_client_hello(const char *hostname,
                           const UINT8 client_random[32],
                           UINT8 *out, int out_cap){
    UINT8 body[512]; int b=0;
    body[b++]=0x03; body[b++]=0x03;                 /* client_version */
    mcp(body+b, client_random, 32); b+=32;          /* random(32) */
    body[b++]=0;                                    /* session_id len 0 */
    put_u16(body+b, 2); b+=2;                       /* cipher_suites len */
    put_u16(body+b, 0xCCA8); b+=2;                  /* ECDHE-RSA-CHACHA20-POLY1305 */
    body[b++]=1; body[b++]=0x00;                    /* compression: null */

    UINT8 ext[400]; int e=0;
    /* supported_groups (0x000a) */
    { UINT8 sg[4]; int s=0;
      put_u16(sg+s, 2); s+=2;
      put_u16(sg+s, 0x0017); s+=2;                  /* secp256r1 */
      put_u16(ext+e, 0x000a); e+=2;
      put_u16(ext+e, s); e+=2;
      mcp(ext+e, sg, s); e+=s;
    }
    /* ec_point_formats (0x000b) */
    { UINT8 pf[2]; int s=0;
      pf[s++]=1; pf[s++]=0x00;
      put_u16(ext+e, 0x000b); e+=2;
      put_u16(ext+e, s); e+=2;
      mcp(ext+e, pf, s); e+=s;
    }
    /* signature_algorithms (0x000d) */
    { UINT8 sa[10]; int s=0;
      put_u16(sa+s, 4); s+=2;
      put_u16(sa+s, 0x0403); s+=2;                  /* ecdsa_secp256r1_sha256 */
      put_u16(sa+s, 0x0401); s+=2;                  /* rsa_pkcs1_sha256 */
      put_u16(ext+e, 0x000d); e+=2;
      put_u16(ext+e, s); e+=2;
      mcp(ext+e, sa, s); e+=s;
    }
    /* server_name (0x0000). RFC 6066 forbids IP literals in SNI, and OpenSSL
     * rejects a ClientHello that carries one, so skip it for IP hostnames. */
    { int hlen=0; int is_ip=1; int dots=0;
      while(hostname[hlen]){ char c=hostname[hlen];
        if(c=='.') dots++; else if(c<'0'||c>'9') is_ip=0; hlen++; }
      if(hlen>0 && is_ip && dots>0){
        /* IP literal: do not emit server_name */
      } else {
        UINT8 sn[300]; int s=0;
        put_u16(sn+s, (UINT16)(hlen+5)); s+=2;
        sn[s++]=0x00;
        put_u16(sn+s, (UINT16)hlen); s+=2;
        mcp(sn+s, (const UINT8*)hostname, hlen); s+=hlen;
        put_u16(ext+e, 0x0000); e+=2;
        put_u16(ext+e, s); e+=2;
        mcp(ext+e, sn, s); e+=s;
      }
    }

    /* Append extensions (with their total length prefix) into the handshake body. */
    put_u16(body+b, (UINT16)e); b+=2;
    mcp(body+b, ext, e); b+=e;

    UINT8 hs[600]; int h=0;
    hs[h++]=0x01;                                   /* ClientHello */
    hs[h++]=(UINT8)((b>>16)&0xff);
    hs[h++]=(UINT8)((b>>8)&0xff);
    hs[h++]=(UINT8)(b&0xff);
    mcp(hs+h, body, b); h+=b;

    int total = h;
    if(total + 5 > out_cap) return -1;
    out[0]=0x16; out[1]=0x03; out[2]=0x03;
    out[3]=(UINT8)((total>>8)&0xff); out[4]=(UINT8)(total&0xff);
    mcp(out+5, hs, total);
    return 5 + total;
}
