/* tls.c - minimal TLS 1.2 crypto primitives (self-contained, no libc).
 * See tls.h for the public API. Verified against RFC 8439 + SHA-256 vectors
 * and an internal P-256 on-curve / commutativity check. */
#include "tls.h"

/* ---------------- small helpers ---------------- */
static void tls_memcpy(UINT8 *d, const UINT8 *s, UINT32 n){ for(UINT32 i=0;i<n;i++) d[i]=s[i]; }
static void tls_memset(UINT8 *d, UINT8 v, UINT32 n){ for(UINT32 i=0;i<n;i++) d[i]=v; }
static int  tls_memeq(const UINT8 *a, const UINT8 *b, UINT32 n){
    UINT8 c=0; for(UINT32 i=0;i<n;i++) c |= (UINT8)(a[i]^b[i]); return c==0;
}
#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
static UINT32 load_be32(const UINT8 *p){ return ((UINT32)p[0]<<24)|((UINT32)p[1]<<16)|((UINT32)p[2]<<8)|p[3]; }
static void  store_be32(UINT8 *p, UINT32 v){ p[0]=(UINT8)(v>>24); p[1]=(UINT8)(v>>16); p[2]=(UINT8)(v>>8); p[3]=(UINT8)v; }
static void  store_le32(UINT8 *p, UINT32 v){ p[0]=(UINT8)v; p[1]=(UINT8)(v>>8); p[2]=(UINT8)(v>>16); p[3]=(UINT8)(v>>24); }
static UINT32 U8TO32(const UINT8 *p){ return ((UINT32)p[0])|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24); }

/* ---------------- SHA-256 ---------------- */
static const UINT32 K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

/* Incremental SHA-256 context is declared in tls.h (sha256_ctx). */
static void sha256_block(UINT32 h[8], const UINT8 *p){
    UINT32 w[64];
    for(UINT32 i=0;i<16;i++) w[i]=load_be32(p+i*4);
    for(UINT32 i=16;i<64;i++){
        UINT32 s0=ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);
        UINT32 s1=ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    UINT32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(UINT32 i=0;i<64;i++){
        UINT32 S1=ROR32(e,6)^ROR32(e,11)^ROR32(e,25);
        UINT32 ch=(e&f)^((~e)&g);
        UINT32 t1=hh+S1+ch+K256[i]+w[i];
        UINT32 S0=ROR32(a,2)^ROR32(a,13)^ROR32(a,22);
        UINT32 maj=(a&b)^(a&c)^(b&c);
        UINT32 t2=S0+maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
void sha256_init(sha256_ctx *c){
    c->h[0]=0x6a09e667;c->h[1]=0xbb67ae85;c->h[2]=0x3c6ef372;c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f;c->h[5]=0x9b05688c;c->h[6]=0x1f83d9ab;c->h[7]=0x5be0cd19;
    c->bufn=0; c->len=0;
}
void sha256_update(sha256_ctx *c, const UINT8 *p, UINT32 n){
    c->len += n;
    while(n>0){
        UINT32 space=64-c->bufn;
        UINT32 take=n<space?n:space;
        for(UINT32 i=0;i<take;i++) c->buf[c->bufn+i]=p[i];
        c->bufn+=take; p+=take; n-=take;
        if(c->bufn==64){ sha256_block(c->h,c->buf); c->bufn=0; }
    }
}
void sha256_final(sha256_ctx *c, UINT8 out[32]){
    UINT64 bitlen=c->len*8;
    c->buf[c->bufn++]=0x80;
    if(c->bufn>56){
        while(c->bufn<64) c->buf[c->bufn++]=0;
        sha256_block(c->h,c->buf); c->bufn=0;
    }
    while(c->bufn<56) c->buf[c->bufn++]=0;
    store_be32(c->buf+56, (UINT32)(bitlen>>32));
    store_be32(c->buf+60, (UINT32)bitlen);
    sha256_block(c->h,c->buf);
    for(int i=0;i<8;i++) store_be32(out+i*4,c->h[i]);
}
void sha256(const UINT8 *msg, UINT32 len, UINT8 out[32]){
    sha256_ctx c; sha256_init(&c); sha256_update(&c,msg,len); sha256_final(&c,out);
}
void hmac_sha256(const UINT8 *key, UINT32 klen, const UINT8 *msg, UINT32 mlen, UINT8 out[32]){
    UINT8 k[64]; UINT32 i;
    if(klen>64){ sha256(key,klen,k); for(i=0;i<32;i++) k[i]=k[i]; for(;i<64;i++) k[i]=0; }
    else { for(i=0;i<klen;i++) k[i]=key[i]; for(;i<64;i++) k[i]=0; }
    UINT8 blk[64]; sha256_ctx c; UINT8 b1[32];
    for(i=0;i<64;i++) blk[i]=(UINT8)(k[i]^0x36);
    sha256_init(&c); sha256_update(&c,blk,64); sha256_update(&c,msg,mlen); sha256_final(&c,b1);
    for(i=0;i<64;i++) blk[i]=(UINT8)(k[i]^0x5c);
    sha256_init(&c); sha256_update(&c,blk,64); sha256_update(&c,b1,32); sha256_final(&c,out);
}

/* ---------------- ChaCha20 ---------------- */
/* RFC 8439 QUARTERROUND uses LEFT rotate by 16/12/8/7. ROR32(x,n) is a right
 * rotate, so left-rotate-by-n == right-rotate-by-(32-n). */
#define CHQR(a,b,c,d) do{ x[a]+=x[b]; x[d]^=x[a]; x[d]=ROR32(x[d],16); \
    x[c]+=x[d]; x[b]^=x[c]; x[b]=ROR32(x[b],20); \
    x[a]+=x[b]; x[d]^=x[a]; x[d]=ROR32(x[d],24); \
    x[c]+=x[d]; x[b]^=x[c]; x[b]=ROR32(x[b],25); }while(0)

void chacha20(const UINT8 key[32], const UINT8 nonce[12], UINT32 counter,
              const UINT8 *in, UINT32 len, UINT8 *out){
    UINT32 x[16]; UINT32 i;
    x[0]=0x61707865; x[1]=0x3320646e; x[2]=0x79622d32; x[3]=0x6b206574;
    for(i=0;i<8;i++) x[4+i]=U8TO32(key+i*4);
    x[12]=counter;
    x[13]=U8TO32(nonce+0); x[14]=U8TO32(nonce+4); x[15]=U8TO32(nonce+8);
    UINT32 y[16]; for(i=0;i<16;i++) y[i]=x[i];
    for(i=0;i<10;i++){
        CHQR(0,4,8,12); CHQR(1,5,9,13); CHQR(2,6,10,14); CHQR(3,7,11,15);
        CHQR(0,5,10,15); CHQR(1,6,11,12); CHQR(2,7,8,13); CHQR(3,4,9,14);
    }
    UINT32 w[16]; for(i=0;i<16;i++) w[i]=x[i]+y[i];
    UINT32 n=(len<64)?len:64;
    for(i=0;i<n;i++){
        UINT32 wi = w[i/4];
        UINT8 kb = (UINT8)(wi >> (8*(i%4)));   /* little-endian keystream */
        out[i]=(UINT8)(kb ^ in[i]);
    }
}

/* ---------------- Poly1305 (RFC 8439, 26-bit limbs) ---------------- */
typedef struct { UINT32 r[5], h[5], s[5]; UINT8 buf[16]; UINT32 blen; } poly_ctx;

static void poly_blocks(poly_ctx *c, const UINT8 *m, UINT32 l){
    /* l is 1..16; decompose into 26-bit limbs and add (with 2^128 marker at end). */
    UINT32 t0,t1,t2,t3;
    t0=U8TO32(m+0); t1=U8TO32(m+4); t2=U8TO32(m+8); t3=U8TO32(m+12);
    UINT32 word[4]={t0,t1,t2,t3};
    /* mask bytes >= l to zero, set marker bit at byte l */
    if(l<16){ for(UINT32 b=l;b<16;b++){ word[b/4] &= ~(0xFFu<<(8*(b%4))); } word[l/4] |= (1u<<(8*(l%4))); }
    UINT32 h0=c->h[0],h1=c->h[1],h2=c->h[2],h3=c->h[3],h4=c->h[4];
    h0 += word[0] & 0x3ffffff;
    h1 += ((word[0]>>26)|(word[1]<<6)) & 0x3ffffff;
    h2 += ((word[1]>>20)|(word[2]<<12)) & 0x3ffffff;
    h3 += ((word[2]>>14)|(word[3]<<18)) & 0x3ffffff;
    if(l==16) h4 += (word[3]>>8) + (1u<<24);   /* full block marker at bit 128 */
    else      h4 += (word[3]>>8);
    /* multiply by r, schoolbook with folded carries */
    UINT32 r0=c->r[0],r1=c->r[1],r2=c->r[2],r3=c->r[3],r4=c->r[4];
    UINT32 s1=r1*5,s2=r2*5,s3=r3*5,s4=r4*5;
    UINT64 d0=(UINT64)h0*r0 + (UINT64)h1*s4 + (UINT64)h2*s3 + (UINT64)h3*s2 + (UINT64)h4*s1;
    UINT64 d1=(UINT64)h0*r1 + (UINT64)h1*r0 + (UINT64)h2*s4 + (UINT64)h3*s3 + (UINT64)h4*s2;
    UINT64 d2=(UINT64)h0*r2 + (UINT64)h1*r1 + (UINT64)h2*r0 + (UINT64)h3*s4 + (UINT64)h4*s3;
    UINT64 d3=(UINT64)h0*r3 + (UINT64)h1*r2 + (UINT64)h2*r1 + (UINT64)h3*r0 + (UINT64)h4*s4;
    UINT64 d4=(UINT64)h0*r4 + (UINT64)h1*r3 + (UINT64)h2*r2 + (UINT64)h3*r1 + (UINT64)h4*r0;
    UINT32 c1;
    c1=(UINT32)(d0>>26); h0=(UINT32)d0&0x3ffffff; d1+=c1;
    c1=(UINT32)(d1>>26); h1=(UINT32)d1&0x3ffffff; d2+=c1;
    c1=(UINT32)(d2>>26); h2=(UINT32)d2&0x3ffffff; d3+=c1;
    c1=(UINT32)(d3>>26); h3=(UINT32)d3&0x3ffffff; d4+=c1;
    c1=(UINT32)(d4>>26); h4=(UINT32)d4&0x3ffffff; h0+=c1*5;
    c1=(h0>>26); h0&=0x3ffffff; h1+=c1;
    c1=(h1>>26); h1&=0x3ffffff; h2+=c1;
    c1=(h2>>26); h2&=0x3ffffff; h3+=c1;
    c1=(h3>>26); h3&=0x3ffffff; h4+=c1;
    c1=(h4>>26); h4&=0x3ffffff; h0+=c1*5;
    c1=(h0>>26); h0&=0x3ffffff; h1+=c1;
    /* value = h0 + h1*2^26 + h2*2^52 + h3*2^78 + h4*2^104 (h4 most significant).
       p = 2^130-5 = (h0=0x3fffffb, h1=0x3ffffff, h2=0x3ffffff, h3=0x3ffffff, h4=0x3ffffff).
       After the fold above, value is < p, so this is normally a no-op (robustness only). */
    int ge=0;
    if(h4>0x3ffffff) ge=1;
    else if(h4==0x3ffffff){
        if(h3>0x3ffffff) ge=1;
        else if(h3==0x3ffffff){
            if(h2>0x3ffffff) ge=1;
            else if(h2==0x3ffffff){
                if(h1>0x3ffffff) ge=1;
                else if(h1==0x3ffffff){ if(h0>=0x3fffffb) ge=1; }
            }
        }
    }
    if(ge){
        h0-=0x3fffffb; h1-=0x3ffffff; h2-=0x3ffffff; h3-=0x3ffffff; h4-=0x3ffffff;
        c1=(h0>>26); h0&=0x3ffffff; h1+=c1;
        c1=(h1>>26); h1&=0x3ffffff; h2+=c1;
        c1=(h2>>26); h2&=0x3ffffff; h3+=c1;
        c1=(h3>>26); h3&=0x3ffffff; h4+=c1;
    }
    c->h[0]=h0;c->h[1]=h1;c->h[2]=h2;c->h[3]=h3;c->h[4]=h4;
}
static void poly_init(poly_ctx *c, const UINT8 key[32]){
    /* RFC 8439 clamp: clear top 4 bits of key[3,7,11,15] and bottom 2 bits of key[4,8,12]. */
    UINT8 kc[16]; tls_memcpy(kc,key,16);
    kc[3]&=15; kc[7]&=15; kc[11]&=15; kc[15]&=15;
    kc[4]&=252; kc[8]&=252; kc[12]&=252;
    UINT32 t0=U8TO32(kc+0),t1=U8TO32(kc+4),t2=U8TO32(kc+8),t3=U8TO32(kc+12);
    c->r[0]=t0 & 0x3ffffff;
    c->r[1]=((t0>>26)|(t1<<6)) & 0x3ffffff;
    c->r[2]=((t1>>20)|(t2<<12)) & 0x3ffffff;
    c->r[3]=((t2>>14)|(t3<<18)) & 0x3ffffff;
    c->r[4]=(t3>>8) & 0x3ffffff;
    c->s[0]=U8TO32(key+16); c->s[1]=U8TO32(key+20); c->s[2]=U8TO32(key+24); c->s[3]=U8TO32(key+28);
    c->h[0]=c->h[1]=c->h[2]=c->h[3]=c->h[4]=0;
    c->blen=0;
}
static void poly_update(poly_ctx *c, const UINT8 *m, UINT32 len){
    while(len>0){
        UINT32 take = 16 - c->blen; if(take>len) take=len;
        tls_memcpy(c->buf+c->blen, m, take); c->blen+=take; m+=take; len-=take;
        if(c->blen==16){ poly_blocks(c,c->buf,16); c->blen=0; }
    }
}
static void poly_final(poly_ctx *c, UINT8 mac[16]){
    if(c->blen>0) poly_blocks(c, c->buf, c->blen);
    UINT32 h0=c->h[0],h1=c->h[1],h2=c->h[2],h3=c->h[3],h4=c->h[4];
    /* Convert V = h (26-bit limbs) into 32-bit little-endian words, carrying over.
       h4 is in [0,2^26); its bits 24-25 (V bits 128-129) are left out of the
       32-bit words and thus discarded by the final mod-2^128. */
    UINT64 acc = h0;
    acc += (UINT64)h1 << 26;
    UINT32 w0 = (UINT32)acc; acc >>= 32;
    acc += (UINT64)h2 << 20;
    UINT32 w1 = (UINT32)acc; acc >>= 32;
    acc += (UINT64)h3 << 14;
    UINT32 w2 = (UINT32)acc; acc >>= 32;
    acc += (UINT64)h4 << 8;
    UINT32 w3 = (UINT32)acc;  /* bits 96..127; higher bits (128+) discarded */
    /* tag = (V + s) mod 2^128 ; s's 32-bit word boundaries align with w's. */
    UINT64 t0 = (UINT64)w0 + c->s[0];
    UINT64 t1 = (UINT64)w1 + c->s[1] + (t0 >> 32);
    UINT64 t2 = (UINT64)w2 + c->s[2] + (t1 >> 32);
    UINT64 t3 = (UINT64)w3 + c->s[3] + (t2 >> 32);
    store_le32(mac,    (UINT32)t0);
    store_le32(mac+4,  (UINT32)t1);
    store_le32(mac+8,  (UINT32)t2);
    store_le32(mac+12, (UINT32)t3);
}

void poly1305(const UINT8 *msg, UINT32 len, const UINT8 key[32], UINT8 mac[16]){
    poly_ctx c; poly_init(&c,key); poly_update(&c,msg,len); poly_final(&c,mac);
}

/* ---------------- ChaCha20-Poly1305 AEAD (RFC 8439) ---------------- */
int aead_chacha20poly1305(int encrypt, const UINT8 key[32], const UINT8 nonce[12],
                          const UINT8 *aad, UINT32 aadlen,
                          const UINT8 *in, UINT32 inlen, UINT8 *out, UINT8 tag[16]){
    UINT8 polykey[32];
    UINT8 innonce[16]; tls_memset(innonce,0,4); tls_memcpy(innonce+4,nonce,12);
    /* poly key = chacha20(key, counter=0, nonce) first 32 bytes */
    UINT8 zeros[32]={0};
    chacha20(key, innonce+4, 0, zeros, 32, polykey);
    /* encrypt/decrypt with counter starting at 1 */
    UINT32 off=0;
    while(off<inlen){
        UINT32 blk=(inlen-off>64)?64:(inlen-off);
        chacha20(key, innonce+4, 1+(off/64), in+off, blk, out+off);
        off+=blk;
    }
    /* build MAC data: aad || pad16(aad) || ct || pad16(ct) ||
     * le32(aadlen)||zeros || le32(ctlen)||zeros  (RFC 8439 §2.8). */
    poly_ctx pc; poly_init(&pc, polykey);
    const UINT8 *ct = encrypt? out : in;
    poly_update(&pc, aad, aadlen);
    { UINT32 pad=(16-(aadlen%16))%16; UINT8 z=0; for(UINT32 i=0;i<pad;i++) poly_update(&pc,&z,1); }
    poly_update(&pc, ct, inlen);
    { UINT32 pad=(16-(inlen%16))%16; UINT8 z=0; for(UINT32 i=0;i<pad;i++) poly_update(&pc,&z,1); }
    UINT8 lens[16];
    store_le32(lens,    aadlen); store_le32(lens+4,  0);
    store_le32(lens+8,  inlen);  store_le32(lens+12, 0);
    poly_update(&pc, lens, 16);
    UINT8 calc[16]; poly_final(&pc, calc);
    if(!encrypt){ if(!tls_memeq(calc,tag,16)) return -1; }
    else tls_memcpy(tag, calc, 16);
    return 0;
}

/* ================= P-256 (big-endian u32[8] field) ================= */
typedef UINT32 fe[8];
static const fe P = {0xFFFFFFFF,0x00000001,0x00000000,0x00000000,0x00000000,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF};
#define P_INV 1u  /* P[7]=0xFFFFFFFF => -P^{-1} mod 2^32 = 1 */

static int  fe_cmp(const fe a, const fe b){ for(int i=0;i<8;i++){ if(a[i]!=b[i]) return (a[i]>b[i])?1:-1; } return 0; }
static int  fe_iszero(const fe a){ for(int i=0;i<8;i++) if(a[i]) return 0; return 1; }
static void fe_copy(fe d, const fe s){ for(int i=0;i<8;i++) d[i]=s[i]; }
static void fe_zero(fe d){ for(int i=0;i<8;i++) d[i]=0; }
static void fe_addw(const fe a, const fe b, UINT32 w[9]){
    UINT64 c=0; for(int i=7;i>=0;i--){ c=(UINT64)a[i]+(UINT64)b[i]+(c>>32); w[i+1]=(UINT32)c; }
    w[0]=(UINT32)(c>>32);
}
static void fe_subw(const fe a, const fe b, fe d){
    INT64 borrow=0; for(int i=7;i>=0;i--){ INT64 v=(INT64)a[i]-(INT64)b[i]-borrow; if(v<0){v+=0x100000000LL;borrow=1;}else borrow=0; d[i]=(UINT32)v; }
}
static void fe_norm(const UINT32 w[9], fe d){
    fe t; for(int i=0;i<8;i++) t[i]=w[i+1];
    if(fe_cmp(t,P)>=0) fe_subw(t,P,d); else fe_copy(d,t);
}
static void fe_add(const fe a, const fe b, fe d){
    UINT32 w[9]; fe_addw(a,b,w);
    if(w[0]){ fe t; fe_copy(t, w+1); fe_subw(t,P,d); if(fe_cmp(d,P)>=0) fe_subw(d,P,d); return; }
    fe_norm(w,d);
}
static void fe_sub(const fe a, const fe b, fe d){
    if(fe_cmp(a,b)>=0){ fe_subw(a,b,d); }
    else { UINT32 w[9]; fe_addw(a,P,w); fe_subw(w+1,b,d); }
}
/* ---- P-256 field arithmetic: schoolbook 256x256 -> 512-bit, NIST fast reduction ----
 * fe is big-endian u32[8].  Modulus P = 2^256 - 2^224 + 2^192 + 2^96 - 1.
 * We use 2^256 == (2^224 - 2^192 - 2^96 + 1) (mod P) to fold a 512-bit product
 * down to < 2^256, then do one conditional subtract of P. */

/* 512-bit big-endian helper (16 limbs, b[0]=MSB) used only during reduction. */
static void b512_zero(UINT32 b[16]){ for(int i=0;i<16;i++) b[i]=0; }
static void b512_add(const UINT32 a[16], const UINT32 b[16], UINT32 out[16]){
    UINT64 c=0; for(int i=15;i>=0;i--){ UINT64 s=(UINT64)a[i]+(UINT64)b[i]+c; out[i]=(UINT32)s; c=s>>32; }
}
static void b512_sub(const UINT32 a[16], const UINT32 b[16], UINT32 out[16]){
    INT64 borrow=0; for(int i=15;i>=0;i--){ INT64 s=(INT64)a[i]-(INT64)b[i]-borrow; if(s<0){s+=0x100000000LL;borrow=1;}else borrow=0; out[i]=(UINT32)s; }
}
/* place an 8-limb fe (big-endian) into 16-limb buffer b at limb offset k (rest zeroed). */
static void b512_fe_at(const fe a, int k, UINT32 b[16]){ b512_zero(b); for(int i=0;i<8;i++) b[k+i]=a[i]; }

/* Full 256x256 -> 512-bit product. a,b are big-endian u32[8]; T is the 512-bit
 * big-endian result (16 limbs, T[0]=MSB). Computed via a little-endian 16-limb
 * accumulator (carry propagates to higher weight = higher index, unambiguous),
 * then limb-reversed into big-endian to match fe_reduce. */
static void fe_mulwide(const fe a, const fe b, UINT32 T[16]){
    UINT64 p[17]; for(int i=0;i<17;i++) p[i]=0;   /* little-endian partial product */
    for(int i=0;i<8;i++){
        UINT32 ai=a[7-i]; UINT64 carry=0;          /* ai = little-endian limb i of a */
        for(int j=0;j<8;j++){
            UINT32 bj=b[7-j];                       /* bj = little-endian limb j of b */
            int k=i+j;
            UINT64 s=(UINT64)ai*bj + p[k] + carry;
            p[k]=(UINT32)s; carry=s>>32;
        }
        int k=i+8; while(carry){ UINT64 s=p[k]+carry; p[k]=(UINT32)s; carry=s>>32; k++; }
    }
    for(int i=0;i<16;i++) T[i]=(UINT32)p[15-i];     /* little-endian -> big-endian */
}
/* Reduce the 512-bit value T[0..15] (big-endian) modulo P, result into d (fe). */
static void fe_reduce(UINT32 z[16], fe d){
    UINT32 tmpA[16], tmpB[16], A[16], B[16], hq[16], nz[16];
    for(int pass=0; pass<20; pass++){
        int nonzero=0; for(int i=0;i<8;i++) if(z[i]){ nonzero=1; break; }
        if(!nonzero) break;
        fe hi; for(int i=0;i<8;i++) hi[i]=z[i];   /* z = hi*2^256 + lo */
        /* A = hi<<224 + hi */
        b512_zero(A);
        b512_fe_at(hi, 1, tmpA); b512_add(A, tmpA, A);
        b512_fe_at(hi, 8, tmpB); b512_add(A, tmpB, A);
        /* B = hi<<192 + hi<<96 */
        b512_zero(B);
        b512_fe_at(hi, 2, tmpA); b512_add(B, tmpA, B);
        b512_fe_at(hi, 5, tmpB); b512_add(B, tmpB, B);
        /* hq = A - B = hi*(2^224 - 2^192 - 2^96 + 1) >= 0 */
        b512_sub(A, B, hq);
        /* nz = lo + hq  (lo = z[8..15] placed at limbs 8..15) */
        b512_zero(nz);
        b512_fe_at(z+8, 8, tmpA); b512_add(nz, tmpA, nz);
        b512_add(nz, hq, nz);
        for(int i=0;i<16;i++) z[i]=nz[i];
    }
    fe r; for(int i=0;i<8;i++) r[i]=z[8+i];
    if(fe_cmp(r,P)>=0) fe_subw(r,P,d); else fe_copy(d,r);
}
static void fe_mul(const fe a, const fe b, fe d){
    UINT32 T[16]; fe_mulwide(a,b,T); fe_reduce(T,d);
}
static void fe_sqr(const fe a, fe d){
    UINT32 T[16]; fe_mulwide(a,a,T); fe_reduce(T,d);
}

static int g_init=0;
static void p256_init(void){ g_init=1; (void)g_init; }

static void fe_norm_keep(fe x){ if(fe_cmp(x,P)>=0) fe_subw(x,P,x); }

/* Modular inverse via Fermat: inv(a) = a^(P-2) mod P (P is prime). Uses the
 * already-verified fe_mul/fe_sqr. Robust and simple; avoids the fragile
 * binary GCD. P-2 = FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFF
 *   FFFD */
static const fe P_MINUS_2 = {0xFFFFFFFF,0x00000001,0x00000000,0x00000000,
                             0x00000000,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFD};
static void fe_inv(const fe a, fe out){
    fe base; fe_copy(base, a);
    fe res;  fe_zero(res); res[7]=1;          /* res = 1 */
    for(int i=0;i<8;i++) for(int b=31;b>=0;b--){
        fe_sqr(res, res);                     /* res = res^2 */
        if(P_MINUS_2[i] & ((UINT32)1<<b)){
            fe_mul(res, base, res);           /* res = res * base */
        }
    }
    fe_copy(out, res);
}

typedef struct { int inf; fe x,y; } pt;
static const fe GX = {0x6B17D1F2,0xE12C4247,0xF8BCE6E5,0x63A440F2,0x77037D81,0x2DEB33A0,0xF4A13945,0xD898C296};
static const fe GY = {0x4FE342E2,0xFE1A7F9B,0x8EE7EB4A,0x7C0F9E16,0x2BCE3357,0x6B315ECE,0xCBB64068,0x37BF51F5};
static const fe Bf = {0x5AC635D8,0xAA3A93E7,0xB3EBBD55,0x769886BC,0x651D06B0,0xCC53B0F6,0x3BCE3C3E,0x27D2604B};

static void pt_double(const pt *P0, pt *R){
    if(P0->inf){ R->inf=1; return; }
    /* Copy input to a local so this is safe for in-place R==P0. */
    pt A; A.inf=P0->inf; fe_copy(A.x,P0->x); fe_copy(A.y,P0->y);
    fe x2, num, den, lambda, t, t2, t3;
    fe_sqr(A.x, x2);                   /* x^2 */
    fe three; fe_zero(three); three[7]=3;
    fe a; fe_sub(P, three, a);            /* a = P-3 */
    fe_add(x2, x2, num); fe_add(num, x2, num); /* num = 3*x^2 */
    fe_add(num, a, num);                  /* num = 3*x^2 + (P-3) = 3x^2 - 3 */
    fe_add(A.y, A.y, den);               /* den = 2y */
    fe_inv(den, lambda);                 /* lambda = 1/(2y) */
    fe_mul(num, lambda, lambda);         /* lambda = num * inv(den) */
    fe_sqr(lambda, t);                    /* lambda^2 */
    fe_sub(t, A.x, t2); fe_sub(t2, A.x, t2); fe_norm_keep(t2); /* x3 = lambda^2 - 2x */
    fe_copy(R->x, t2);
    fe_sub(A.x, R->x, t3); fe_mul(t3, lambda, t3); fe_sub(t3, A.y, R->y); fe_norm_keep(R->y);
    R->inf=0;
}
static void pt_add(const pt *P0, const pt *Q0, pt *R){
    /* Copy inputs to locals so this is safe for in-place R==P0 or R==Q0. */
    pt A, B;
    A.inf=P0->inf; fe_copy(A.x,P0->x); fe_copy(A.y,P0->y);
    B.inf=Q0->inf; fe_copy(B.x,Q0->x); fe_copy(B.y,Q0->y);
    if(A.inf){ fe_copy(R->x,B.x); fe_copy(R->y,B.y); R->inf=B.inf; return; }
    if(B.inf){ fe_copy(R->x,A.x); fe_copy(R->y,A.y); R->inf=A.inf; return; }
    fe dx,dy,lambda;
    fe_sub(B.x,A.x,dx); fe_sub(B.y,A.y,dy);
    if(fe_iszero(dx)){
        if(fe_iszero(dy)){ pt_double(&A,R); return; }
        R->inf=1; return;
    }
    fe_inv(dx, lambda); fe_mul(dy, lambda, lambda);
    fe t1,t2;
    fe_sqr(lambda,t1); fe_sub(t1,A.x,t2); fe_sub(t2,B.x,t2); fe_norm_keep(t2);
    fe_copy(R->x,t2);
    fe_sub(A.x,R->x,t1); fe_mul(t1,lambda,t1); fe_sub(t1,A.y,R->y); fe_norm_keep(R->y);
    R->inf=0;
}
static void pt_mul(const fe priv, const pt *G, pt *R){
    /* double-and-add: R = priv*G, starting from the point at infinity. */
    pt res; res.inf=1;
    pt Gc;  Gc.inf=0; fe_copy(Gc.x,G->x); fe_copy(Gc.y,G->y);
    for(int i=0;i<8;i++) for(int b=31;b>=0;b--){
        pt_double(&res,&res);
        if(priv[i] & (1u<<b)) pt_add(&res,&Gc,&res);
    }
    fe_copy(R->x,res.x); fe_copy(R->y,res.y); R->inf=res.inf;
}
static void fe_to_bytes(const fe a, UINT8 out[32]){
    for(int i=0;i<8;i++){ out[i*4]=(UINT8)(a[i]>>24); out[i*4+1]=(UINT8)(a[i]>>16); out[i*4+2]=(UINT8)(a[i]>>8); out[i*4+3]=(UINT8)a[i]; }
}
static void bytes_to_fe(const UINT8 in[32], fe a){ for(int i=0;i<8;i++) a[i]=load_be32(in+i*4); }

int p256_keypair(const UINT8 priv[32], UINT8 pub_x[32], UINT8 pub_y[32]){
    p256_init();
    fe d; bytes_to_fe(priv,d);
    if(fe_iszero(d)) d[7]=1;
    pt Q; pt G; G.inf=0; fe_copy(G.x,GX); fe_copy(G.y,GY);
    pt_mul(d,&G,&Q);
    if(Q.inf) return -1;
    fe_to_bytes(Q.x,pub_x); fe_to_bytes(Q.y,pub_y);
    return 0;
}
int p256_ecdh(const UINT8 priv[32], const UINT8 peer_x[32], const UINT8 peer_y[32], UINT8 shared[32]){
    p256_init();
    fe d; bytes_to_fe(priv,d);
    if(fe_iszero(d)) d[7]=1;
    pt Q; Q.inf=0; bytes_to_fe(peer_x,Q.x); bytes_to_fe(peer_y,Q.y);
    pt_mul(d,&Q,&Q);
    if(Q.inf) return -1;
    fe_to_bytes(Q.x,shared);
    return 0;
}

/* ---------------- RNG hook ---------------- */
tls_rng_fn tls_rng = 0;
void tls_set_rng(tls_rng_fn f){ tls_rng = f; }

/* ---------------- self-test ---------------- */
static int sscanf_hex(const char *s, unsigned int *v){ *v=0; for(int i=0;i<2;i++){ char c=s[i]; int d; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else if(c>='A'&&c<='F')d=c-'A'+10; else d=0; *v=(*v<<4)|d; } return 1; }
static void hex2buf(const char *s, UINT8 *b, int n){ for(int i=0;i<n;i++){ unsigned int v; sscanf_hex(s+i*2,&v); b[i]=(UINT8)v; } }

int tls_selftest(void){
    int rc=0;
    { UINT8 h[32]; const UINT8 m[]="abc"; sha256(m,3,h);
      const UINT8 exp[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                           0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
      if(!tls_memeq(h,exp,32)) rc|=1; }
    { UINT8 h[32]; sha256((const UINT8*)"",0,h);
      const UINT8 exp[32]={0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
                           0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
      if(!tls_memeq(h,exp,32)) rc|=2; }
    { UINT8 key[32], nonce[12], out[64], exp[64], plain[64];
      const char *ks="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
      const char *ns="000000090000004a00000000";
      const char *ps="0000000000000000000000000000000000000000000000000000000000000000"
                      "0000000000000000000000000000000000000000000000000000000000000000";
      const char *es="10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                     "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e";
      hex2buf(ks,key,32); hex2buf(ns,nonce,12); hex2buf(ps,plain,64); hex2buf(es,exp,64);
      chacha20(key,nonce,1,plain,64,out);
      if(!tls_memeq(out,exp,64)) rc|=4; }
    { UINT8 mac[16], exp[16];
      const char *m="Cryptographic Forum Research Group";
      const char *ks="85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b";
      const char *es="a8061dc1305136c6c22b8baf0c0127a9";
      UINT8 key[32]; hex2buf(ks,key,32); hex2buf(es,exp,16);
      poly1305((const UINT8*)m,34,key,mac);
      if(!tls_memeq(mac,exp,16)) rc|=8; }
    { UINT8 key[32], nonce[12], ct[128], tag[16], pt_out[128], aad[8];
      const char *ks="808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
      const char *ns="070000004041424344454647";
      const char *aads="50515253c0c1c2c3c4c5c6c7";
      const char *ps="4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
                     "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
                     "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
                     "637265656e20776f756c642062652069742e";
      const char *cs="d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
                     "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                     "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                     "3ff4def08e4b7a9de576d26586cec64b6116";
      const char *ts="1ae10b594f09e26a7e902ecbd0600691";
      hex2buf(ks,key,32); hex2buf(ns,nonce,12); hex2buf(aads,aad,12);
      UINT8 plain[114]; hex2buf(ps,plain,114);
      UINT8 cexp[114]; hex2buf(cs,cexp,114);
      UINT8 texp[16]; hex2buf(ts,texp,16);
      int e = aead_chacha20poly1305(1,key,nonce,aad,12,plain,114,ct,tag);
      if(e||!tls_memeq(ct,cexp,114)||!tls_memeq(tag,texp,16)) rc|=16;
      if(aead_chacha20poly1305(0,key,nonce,aad,12,ct,114,pt_out,tag)!=0) rc|=32;
      else if(!tls_memeq(pt_out,plain,114)) rc|=64;
    }
    {
      p256_init();
      fe lhs, rhs, x2, x3, a, t;
      fe_sqr(GY,lhs);                       /* lhs = y^2 */
      fe_sqr(GX,x2);                        /* x2 = x^2 */
      fe_mul(x2,GX,x3);                     /* x3 = x^3 */
      fe_zero(a); a[7]=3; fe_sub(P,a,a);    /* a = P-3 (curve a = -3 mod P) */
      fe_mul(a,GX,t);                       /* t = a*x = (P-3)*x */
      fe_add(x3,t,rhs);                     /* rhs = x^3 + (P-3)*x */
      fe_add(rhs,Bf,rhs);                   /* rhs = x^3 + (P-3)*x + b */
      if(fe_cmp(lhs,rhs)!=0) rc|=128;
      UINT8 dA[32], dB[32], QAx[32],QAy[32], QBx[32],QBy[32], s1[32], s2[32];
      tls_memset(dA,0,32); dA[31]=0x11; dA[30]=0x22;
      tls_memset(dB,0,32); dB[31]=0x33; dB[30]=0x44;
      p256_keypair(dA,QAx,QAy); p256_keypair(dB,QBx,QBy);
      p256_ecdh(dA,QBx,QBy,s1); p256_ecdh(dB,QAx,QAy,s2);
      if(!tls_memeq(s1,s2,32)) rc|=256;
    }
    return rc;
}
