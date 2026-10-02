/* tls_bn.c - minimal big-integer arithmetic for RSA (up to 2048-bit).
 * See tls_bn.h. Little-endian 32-bit limbs, no libc. */
#include "tls_bn.h"

static int bmemcmp(const UINT8 *a, const UINT8 *b, int n){
    for(int i=0;i<n;i++) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return 0;
}

void bn_zero(bn *a){ for(int i=0;i<BN_LIMBS;i++) a->d[i]=0; a->n=1; }
void bn_one(bn *a){ for(int i=0;i<BN_LIMBS;i++) a->d[i]=0; a->d[0]=1; a->n=1; }
void bn_copy(bn *dst, const bn *src){ for(int i=0;i<BN_LIMBS;i++) dst->d[i]=src->d[i]; dst->n=src->n; }

void bn_from_be(bn *a, const UINT8 *be, int len){
    for(int i=0;i<BN_LIMBS;i++) a->d[i]=0;
    for(int i=0;i<len;i++){
        int shift = (len-1-i)*8;
        int limb = shift/32, bit = shift%32;
        a->d[limb] |= ((UINT32)be[i]) << bit;
    }
    a->n = BN_LIMBS; while(a->n>1 && a->d[a->n-1]==0) a->n--;
}

void bn_to_be(const bn *a, UINT8 *out, int len){
    /* assumes a->d beyond a->n are zero (true after from_be / arithmetic) */
    for(int i=0;i<len;i++){
        int shift = (len-1-i)*8;
        int limb = shift/32, bit = shift%32;
        UINT32 v = (limb < a->n) ? a->d[limb] : 0;
        out[i] = (UINT8)((v >> bit) & 0xff);
    }
}

int bn_cmp(const bn *a, const bn *b){
    if(a->n != b->n) return a->n < b->n ? -1 : 1;
    for(int i=a->n-1;i>=0;i--){
        if(a->d[i]!=b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

int bn_is_zero(const bn *a){
    if(a->n!=1) return 0;
    return a->d[0]==0;
}

void bn_add(bn *r, const bn *a, const bn *b){
    UINT64 carry=0; int m = a->n>b->n?a->n:b->n;
    for(int i=0;i<m;i++){
        UINT64 s = (UINT64)(i<a->n?a->d[i]:0) + (i<b->n?b->d[i]:0) + carry;
        r->d[i] = (UINT32)(s & 0xffffffffULL);
        carry = s >> 32;
    }
    if(carry){ r->d[m]=(UINT32)carry; m++; }
    r->n = m;
}

int bn_sub(bn *r, const bn *a, const bn *b){
    if(bn_cmp(a,b)<0) return -1;
    INT64 borrow=0;
    int m=a->n;
    for(int i=0;i<m;i++){
        INT64 s = (INT64)(a->d[i]) - (INT64)(i<b->n?b->d[i]:0) - borrow;
        if(s<0){ s += ((INT64)1<<32); borrow=1; } else borrow=0;
        r->d[i] = (UINT32)s;
    }
    while(m>1 && r->d[m-1]==0) m--;
    r->n = m;
    return 0;
}

static void bn_lshift1(bn *a){
    UINT64 carry=0;
    for(int i=0;i<a->n;i++){
        UINT64 cur=((UINT64)a->d[i]<<1)|carry;
        a->d[i]=(UINT32)(cur&0xffffffffULL);
        carry=cur>>32;
    }
    if(carry){ a->d[a->n]=(UINT32)carry; a->n++; }
}

void bn_mul(bn *r, const bn *a, const bn *b){
    bn t; for(int i=0;i<BN_LIMBS;i++) t.d[i]=0;
    for(int i=0;i<a->n;i++){
        UINT64 carry=0;
        for(int j=0;j<b->n;j++){
            UINT64 cur = (UINT64)t.d[i+j] + (UINT64)a->d[i]*(UINT64)b->d[j] + carry;
            t.d[i+j] = (UINT32)(cur & 0xffffffffULL);
            carry = cur >> 32;
        }
        int k=i+b->n;
        while(carry && k<BN_LIMBS){
            UINT64 cur=(UINT64)t.d[k]+carry;
            t.d[k]=(UINT32)(cur&0xffffffffULL); carry=cur>>32; k++;
        }
    }
    int m=a->n+b->n; if(m>BN_LIMBS) m=BN_LIMBS;
    while(m>1 && t.d[m-1]==0) m--;
    t.n=m; *r=t;
}

void bn_divmod(bn *q, bn *r, const bn *u, const bn *v){
    if(bn_is_zero(v)){ /* undefined; leave */ return; }
    bn qq; bn_zero(&qq);
    bn rr; bn_zero(&rr);
    int hi=-1;
    for(int i=u->n-1;i>=0;i--){
        if(u->d[i]){
            for(int b=31;b>=0;b--){ if((u->d[i]>>b)&1){ hi=i*32+b; break; } }
            break;
        }
    }
    if(hi<0){ if(q) bn_zero(q); if(r) bn_zero(r); return; }
    for(int i=hi;i>=0;i--){
        bn_lshift1(&rr);
        int bit = (u->d[i/32] >> (i%32)) & 1;
        rr.d[0] |= (UINT32)bit;
        if(bn_cmp(&rr,v)>=0){
            bn_sub(&rr,&rr,v);
            int limb=i/32, b=i%32;
            qq.d[limb] |= ((UINT32)1<<b);
        }
    }
    int m=BN_LIMBS; while(m>1 && qq.d[m-1]==0) m--; qq.n=m;
    if(q) *q=qq;
    if(r) *r=rr;
}

void bn_modexp(bn *out, const bn *base, const bn *exp, const bn *mod){
    bn b; bn_copy(&b, base);
    if(bn_cmp(&b,mod)>=0){ bn_divmod(0,&b,&b,mod); }
    bn r; bn_one(&r);
    int hi=-1;
    for(int i=exp->n-1;i>=0;i--){
        if(exp->d[i]){
            for(int bb=31;bb>=0;bb--){ if((exp->d[i]>>bb)&1){ hi=i*32+bb; break; } }
            break;
        }
    }
    if(hi<0){ bn_one(out); return; }
    for(int i=hi;i>=0;i--){
        bn t; bn_mul(&t,&r,&r); bn_divmod(0,&r,&t,mod);
        int bit=(exp->d[i/32]>>(i%32))&1;
        if(bit){ bn_mul(&t,&r,&b); bn_divmod(0,&r,&t,mod); }
    }
    *out=r;
}
