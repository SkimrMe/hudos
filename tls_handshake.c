/* tls_handshake.c - TLS 1.2 client handshake state machine (cipher 0xCCA8).
 * Freestanding: NO libc / NO stdio. See tls_handshake.h for the I/O model.
 *
 * Flow (full handshake, ECDHE-RSA-CHACHA20-POLY1305):
 *   -> ClientHello
 *   <- ServerHello, Certificate+, ServerKeyExchange, ServerHelloDone
 *   -> ClientKeyExchange, ChangeCipherSpec, Finished(client)
 *   <- ChangeCipherSpec, Finished(server)
 */
#include "tls.h"
#include "tls_cli.h"
#include "tls_rsa.h"
#include "x509.h"
#include "tls_handshake.h"

/* debug hook (NULL in kernel) */
tls_dbg_fn tls_dbg = 0;
#define DBG(m) do{ if(tls_dbg) tls_dbg(m); }while(0)

/* ---------- small local helpers ---------- */
static void mcp(UINT8 *d, const UINT8 *s, int n){ for(int i=0;i<n;i++) d[i]=s[i]; }
static void mset(UINT8 *d, UINT8 v, int n){ for(int i=0;i<n;i++) d[i]=v; }
static int  my_cmp(const UINT8 *a, const UINT8 *b, int n){
    for(int i=0;i<n;i++){ if(a[i]!=b[i]) return (int)a[i]-(int)b[i]; } return 0;
}

/* ---------- raw I/O ---------- */
static int io_read_exact(tls_conn *c, UINT8 *buf, int n){
    int got=0;
    while(got<n){
        int r=c->recv(c->ctx, buf+got, n-got);
        if(r<0) return -1;
        if(r==0) return -1;          /* EOF before n */
        got+=r;
    }
    return 0;
}

/* Read one TLS record. If decrypt, the fragment is AEAD-decrypted with the
 * server write key (RFC 7905). On success *flen holds the plaintext length. */
static int read_record(tls_session *s, int decrypt, int *ctype,
                       UINT8 *frag, int *flen){
    UINT8 hdr[5];
    if(io_read_exact(&s->conn, hdr, 5)!=0) return -1;
    *ctype = hdr[0];
    int rlen = ((int)hdr[3]<<8)|hdr[4];
    if(rlen < 0 || rlen > 16384+16) return -1;
    if(io_read_exact(&s->conn, frag, rlen)!=0) return -1;
    if(decrypt){
        if(rlen < 16) return -1;
        int ct = rlen - 16;
        UINT8 plain[16384];
        if(tls_record_decrypt(s->server_write_key, s->server_write_iv, s->srv_seq,
                              (UINT8)hdr[0], hdr, frag, ct, plain)!=0) return -1;
        s->srv_seq++;
        mcp(frag, plain, ct);
        *flen = ct;
    } else {
        *flen = rlen;
    }
    return 0;
}

static void feed_trans(tls_session *s, const UINT8 *p, int n){
    sha256_update(&s->trans, p, (UINT32)n);
}

/* Reassemble one handshake message out of a stream of 0x16 records.
 * If decrypt != 0 the records are AEAD-protected (server Finished).
 * If feed != 0 the message is added to the handshake transcript hash.
 *   The server's Finished must NOT be fed (its verify_data is computed
 *   over the transcript that excludes the Finished itself). */
static int next_hs_msg(tls_session *s, int decrypt, int feed, int *htype,
                       UINT8 *body, int *blen){
    for(;;){
        if(s->hspos+4 <= s->hslen){
            int t = s->hsbuf[s->hspos];
            int len = ((int)s->hsbuf[s->hspos+1]<<16)|
                      ((int)s->hsbuf[s->hspos+2]<<8)|
                      ((int)s->hsbuf[s->hspos+3]);
            if(s->hspos+4+len <= s->hslen){
                if(feed) feed_trans(s, s->hsbuf+s->hspos, 4+len);
                mcp(body, s->hsbuf+s->hspos+4, len);
                *htype = t; *blen = len;
                s->hspos += 4+len;
                return 0;
            }
        }
        int ctype; UINT8 frag[16384+16]; int flen;
        if(read_record(s, decrypt, &ctype, frag, &flen)!=0) return -1;
        if(ctype != 0x16) return -1;          /* expected handshake record */
        int rem = s->hslen - s->hspos;
        if(rem>0) mcp(s->hsbuf, s->hsbuf+s->hspos, rem);
        s->hslen = rem; s->hspos = 0;
        if(s->hslen + flen > (int)sizeof(s->hsbuf)) return -1;
        mcp(s->hsbuf+s->hslen, frag, flen);
        s->hslen += flen;
    }
}

static int send_clear_record(tls_session *s, UINT8 ctype,
                             const UINT8 *data, int len){
    UINT8 hdr[5];
    hdr[0]=ctype; hdr[1]=0x03; hdr[2]=0x03;
    hdr[3]=(UINT8)(len>>8); hdr[4]=(UINT8)len;
    if(s->conn.send(s->conn.ctx, hdr, 5)!=5) return -1;
    if(s->conn.send(s->conn.ctx, data, len)!=len) return -1;
    return 0;
}

/* master_secret + key block */
static void compute_keys(tls_session *s, const UINT8 pre_master[32]){
    UINT8 seed[64]; mcp(seed, s->client_random, 32); mcp(seed+32, s->server_random, 32);
    tls_prf_sha256(pre_master, 32, "master secret", seed, 64, s->master, 48);
    UINT8 kbseed[64]; mcp(kbseed, s->server_random, 32); mcp(kbseed+32, s->client_random, 32);
    /* ChaCha20-Poly1305 key block = 72 bytes:
     * client_write_key(32) || server_write_key(32) ||
     * client_write_IV(4)  || server_write_IV(4) */
    /* RFC 7905: key_block = client_write_key(32) || server_write_key(32) ||
     * client_write_IV(12) || server_write_IV(12)  (fixed_iv_length = 12). */
    UINT8 kb[88]; tls_prf_sha256(s->master, 48, "key expansion", kbseed, 64, kb, 88);
    mcp(s->client_write_key, kb, 32);
    mcp(s->server_write_key, kb+32, 32);
    mcp(s->client_write_iv, kb+64, 12);
    mcp(s->server_write_iv, kb+76, 12);
}

/* ===================================================================== */
int tls_handshake(tls_session *s, const char *hostname, const tls_trust *trust){
    if(!tls_rng) return -1;

    /* ephemeral client key pair (used for ClientKeyExchange + ECDHE) */
    UINT8 client_priv[32], client_pubx[32], client_puby[32];
    if(tls_rng(client_priv, 32)!=32) return -1;
    if(p256_keypair(client_priv, client_pubx, client_puby)!=0) return -1;

    /* client random */
    if(tls_rng(s->client_random, 32)!=32) return -1;

    sha256_init(&s->trans);
    s->hspos=0; s->hslen=0; s->cli_seq=0; s->srv_seq=0;
    s->established=0;

    /* ---- ClientHello ---- */
    UINT8 ch[5+600];
    int chlen = tls_build_client_hello(hostname, s->client_random, ch, (int)sizeof(ch));
    if(chlen<0) return -1;
    feed_trans(s, ch+5, chlen-5);                 /* handshake bytes only */
    if(tls_dbg){
        static const char hexc[]="0123456789abcdef";
        static char dump[1200];
        int di=0;
        dump[di++]='C'; dump[di++]='H'; dump[di++]='(';
        dump[di++]='0'+((chlen/100)%10); dump[di++]='0'+((chlen/10)%10); dump[di++]='0'+(chlen%10);
        dump[di++]=')'; dump[di++]=':';
        for(int i=0;i<chlen && di<(int)sizeof(dump)-2;i++){
            dump[di++]=hexc[(ch[i]>>4)&0xf]; dump[di++]=hexc[ch[i]&0xf];
        }
        dump[di]=0;
        DBG(dump);
    }
    if(s->conn.send(s->conn.ctx, ch, chlen)!=chlen) return -1;

    /* ---- read ServerHello / Certificate / ServerKeyExchange / ServerHelloDone ---- */
    UINT8 leaf_n[513], leaf_e[16]; int leaf_n_len=0, leaf_e_len=0;
    int got_sh=0, got_cert=0, got_ske=0, got_shd=0;
    int n_iter=0;
    while(!(got_sh && got_cert && got_ske && got_shd)){
        if(++n_iter > 16) return -1;
        int ht; UINT8 body[16384]; int bl;
        if(next_hs_msg(s, 0, 1, &ht, body, &bl)!=0) return -1;

        if(ht==0x02){                            /* ServerHello */
            if(bl < 2+32+1+2+1) return -1;
            UINT16 ver = (UINT16)((body[0]<<8)|body[1]);
            if(ver != 0x0303) return -1;         /* require TLS 1.2 */
            mcp(s->server_random, body+2, 32);
            int p = 34;
            int sid_len = body[p++]; p += sid_len;
            if(p+2 > bl) return -1;
            UINT16 cs = (UINT16)((body[p]<<8)|body[p+1]); p+=2;
            if(cs != 0xCCA8) return -1;          /* only suite we support */
            if(p+1 > bl) return -1;
            /* compression method + optional extensions: ignore */
            DBG("SH: ServerHello ok (suite 0xCCA8)");
            got_sh=1;
        }
        else if(ht==0x0b){                        /* Certificate */
            int p=0;
            if(bl < 3) return -1;
            int clen = (body[p]<<16)|(body[p+1]<<8)|body[p+2]; p+=3;
            if(p+clen != bl) return -1;
            x509_cert certs[8]; int nc=0;
            while(p < bl && nc < 8){
                int l = (body[p]<<16)|(body[p+1]<<8)|body[p+2]; p+=3;
                if(p+l > bl) return -1;
                if(x509_parse(body+p, l, &certs[nc])!=0) return -1;
                p+=l; nc++;
            }
            if(nc==0) return -1;
            /* capture leaf RSA public key for ServerKeyExchange verify */
            mcp(leaf_n, certs[0].n, certs[0].n_len); leaf_n_len=certs[0].n_len;
            mcp(leaf_e, certs[0].e, certs[0].e_len); leaf_e_len=certs[0].e_len;
            /* verify chain against the pinned trust store: accept if ANY anchor
             * validates the chain (standard trust-anchor model). */
            int verified=0;
            for(int ai=0; ai<trust->n && ai<TLS_TRUST_MAX; ai++){
                x509_cert root;
                if(x509_parse(trust->certs[ai], trust->lens[ai], &root)!=0) continue;
                if(x509_verify_chain(certs, nc, root.n, root.n_len, root.e, root.e_len)==0){
                    verified=1; break;
                }
            }
            if(!verified){
                DBG("CERT: chain verify FAILED (no trusted anchor)");
                return -1;
            }
            DBG("CERT: chain verify ok");
            got_cert=1;
        }
        else if(ht==0x0c){                        /* ServerKeyExchange */
            if(bl < 4+65+4) return -1;
            if(body[0]!=0x03) return -1;          /* named_curve */
            UINT16 ncurve = (UINT16)((body[1]<<8)|body[2]);
            if(ncurve!=0x0017) return -1;         /* secp256r1 */
            int plen = body[3];
            if(plen!=65) return -1;
            const UINT8 *point = body+4;
            int param_len = 4 + plen;            /* exactly the signed params */
            int sp = 4 + plen;
            if(sp+4 > bl) return -1;
            UINT16 salg = (UINT16)((body[sp]<<8)|body[sp+1]); sp+=2;
            if(salg!=0x0401) return -1;           /* rsa_pkcs1_sha256 */
            int siglen = (body[sp]<<8)|body[sp+1]; sp+=2;
            const UINT8 *sig = body+sp;
            if(sp+siglen != bl) return -1;

            /* signed_data = client_random || server_random || ServerECDHParams */
            UINT8 sd[32+32+4+65];
            mcp(sd, s->client_random, 32);
            mcp(sd+32, s->server_random, 32);
            mcp(sd+64, body, param_len);
            UINT8 dig[32]; sha256(sd, (UINT32)(64+param_len), dig);
            if(rsa_pkcs1_v15_verify_sha256(leaf_n, leaf_n_len,
                                           leaf_e, leaf_e_len,
                                           sig, siglen, dig)!=0){
                DBG("SKE: signature verify FAILED");
                return -1;
            }
            DBG("SKE: signature ok");

            /* ECDHE: shared = priv * peer_pub */
            const UINT8 *px = point+1, *py = point+1+32;
            UINT8 shared[32];
            if(p256_ecdh(client_priv, px, py, shared)!=0){
                DBG("SKE: ECDHE failed");
                return -1;
            }
            compute_keys(s, shared);
            DBG("SKE: ECDHE+keys ok");
            got_ske=1;
        }
        else if(ht==0x0e){                        /* ServerHelloDone */
            got_shd=1;
        }
        else {
            return -1;                            /* unexpected message */
        }
    }

    /* ---- ClientKeyExchange ---- */
    UINT8 ck[4+1+65]; int k=0;
    ck[k++]=0x10;                                /* ClientKeyExchange */
    ck[k++]=0; ck[k++]=0; ck[k++]=(UINT8)(1+65); /* length = 66 */
    ck[k++]=0x41;                                /* point length */
    ck[k++]=0x04;                                /* uncompressed */
    mcp(ck+k, client_pubx, 32); k+=32;
    mcp(ck+k, client_puby, 32); k+=32;
    feed_trans(s, ck, k);
    if(send_clear_record(s, 0x16, ck, k)!=0) return -1;

    /* ---- ChangeCipherSpec ---- */
    UINT8 ccs[1]={0x01};
    if(send_clear_record(s, 0x14, ccs, 1)!=0) return -1;

    /* ---- Finished (client) ---- */
    sha256_ctx tc = s->trans;                    /* hash up to ClientKeyExchange */
    UINT8 hsh[32]; sha256_final(&tc, hsh);
    UINT8 cf[12];
    tls_prf_sha256(s->master, 48, "client finished", hsh, 32, cf, 12);
    UINT8 fin[4+12]; int f=0;
    fin[f++]=0x14; fin[f++]=0; fin[f++]=0; fin[f++]=(UINT8)12;
    mcp(fin+f, cf, 12); f+=12;
    feed_trans(s, fin, 4+12);                    /* now includes client Finished */
    UINT8 rec[5+(4+12)+16]; int rl=0;
    if(tls_record_encrypt(s->client_write_key, s->client_write_iv, s->cli_seq,
                          0x16, fin, 4+12, rec, &rl)!=0)
        return -1;
    s->cli_seq++;
    if(s->conn.send(s->conn.ctx, rec, rl)!=rl) return -1;
    DBG("SENT: client Finished (encrypted)");

    /* ---- server: ChangeCipherSpec ---- */
    int ctype; UINT8 frag[64]; int flen;
    if(read_record(s, 0, &ctype, frag, &flen)!=0) return -1;
    if(ctype!=0x14 || flen!=1 || frag[0]!=0x01) return -1;
    DBG("RECV: server ChangeCipherSpec");

    /* ---- server: Finished (encrypted) ---- */
    int st; UINT8 sfin[64]; int sl;
    if(next_hs_msg(s, 1, 0, &st, sfin, &sl)!=0){
        DBG("RECV: server Finished decrypt/parse FAILED");
        return -1;
    }
    if(st!=0x14 || sl!=12){
        DBG("RECV: server Finished wrong type/len");
        return -1;
    }
    sha256_ctx tc2 = s->trans;                   /* includes client Finished */
    UINT8 hsh2[32]; sha256_final(&tc2, hsh2);
    UINT8 sf[12];
    tls_prf_sha256(s->master, 48, "server finished", hsh2, 32, sf, 12);
    if(my_cmp(sfin, sf, 12)!=0){
        DBG("RECV: server Finished verify_data MISMATCH");
        return -1;
    }
    DBG("RECV: server Finished verify ok");

    s->established = 1;
    return 0;
}

/* ===================================================================== */
int tls_send(tls_session *s, const UINT8 *data, int len){
    if(!s->established) return -1;
    UINT8 rec[5+16384+16]; int rl;
    if(len > 16384) return -1;
    if(tls_record_encrypt(s->client_write_key, s->client_write_iv, s->cli_seq, 0x17,
                          data, len, rec, &rl)!=0) return -1;
    s->cli_seq++;
    if(s->conn.send(s->conn.ctx, rec, rl)!=rl) return -1;
    return len;
}

int tls_recv(tls_session *s, UINT8 *buf, int max){
    if(!s->established) return -1;
    for(;;){
        int ctype; UINT8 frag[16384+16]; int flen;
        if(read_record(s, 1, &ctype, frag, &flen)!=0) return -1;
        if(ctype==0x17){                          /* application_data */
            if(flen>max) flen=max;
            mcp(buf, frag, flen);
            return flen;
        } else if(ctype==0x15){                    /* alert */
            return -1;
        } else if(ctype==0x16){                    /* handshake (renegotiation) */
            /* not supported; skip */
            continue;
        } else {
            return -1;
        }
    }
}

void tls_close(tls_session *s){
    if(!s->established) return;
    /* best-effort close_notify alert */
    UINT8 alert[2]={0x01,0x00};
    UINT8 rec[5+2+16]; int rl;
    if(tls_record_encrypt(s->client_write_key, s->client_write_iv, s->cli_seq, 0x15, alert, 2, rec, &rl)==0){
        s->conn.send(s->conn.ctx, rec, rl);
    }
    s->established=0;
}
