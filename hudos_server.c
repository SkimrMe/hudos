/* hudos-server — a minimal Unix-like OS / shell that runs as a UEFI
 * application (aarch64 / ARM64 EFI). It boots from the ESP, presents a
 * text CLI (ConOut, captured by QEMU -serial), implements ls/cd/mkdir/
 * open/del/help/copy/pwd/cls, and can LOAD & RUN aarch64 ELF executables
 * via the `open` command. Userland programs are freestanding PIE ELF files
 * compiled for this platform; they talk to the OS through a tiny syscall
 * shim passed in register x3 (no SVC / exceptions needed).
 *
 * Build with clang + lld-link (aarch64-pc-win32-coff), see Makefile.
 */
#include "efi.h"
/* CJK + ASCII bitmap fonts, shared with tdesktop so the kernel shell renders
 * Chinese exactly like the desktop does. */
#include "userland/cjk16x16.h"
#include "userland/sun8x16.h"

/* ---- TLS 1.2 client (native, freestanding) ----
 * Headers only; the implementation lives in tls*.c and is compiled as separate
 * freestanding objects and linked into BOOTAA64.EFI (see Makefile). We reuse
 * efi.h's integer typedefs (UINT8..UINT64) and suppress tls.h's own copies. */
#define HUDOS_TLS_TYPES
#include "tls.h"
#include "tls_cli.h"
#include "tls_handshake.h"
#include "tls_roots.h"

/* Minimal libc shims required by the freestanding TLS objects: under -O2 the
 * compiler lowers aggregate copies (e.g. `sha256_ctx tc = s->trans;`) to memcpy
 * calls, and /nodefaultlib provides no C runtime. These resolve those symbols.
 * -fno-builtin is set, so the compiler will not optimize them back to builtins. */
void *memcpy(void *d, const void *s, unsigned long n){
    UINT8 *p=(UINT8*)d; const UINT8 *q=(const UINT8*)s;
    while(n--) *p++=*q++; return d;
}
void *memset(void *d, int v, unsigned long n){
    UINT8 *p=(UINT8*)d; while(n--) *p++=(UINT8)v; return d;
}
void *memmove(void *d, const void *s, unsigned long n){
    UINT8 *p=(UINT8*)d; const UINT8 *q=(const UINT8*)s;
    if(p<q){ while(n--) *p++=*q++; }
    else { p+=n; q+=n; while(n--) *--p=*--q; }
    return d;
}

/* ---- globals ---- */
static EFI_SYSTEM_TABLE         *gST;
static EFI_BOOT_SERVICES        *gBS;
static EFI_RUNTIME_SERVICES     *gRT;
static EFI_HANDLE                gImageHandle;
static EFI_FILE_PROTOCOL        *gRoot = NULL;   /* boot (ESP) volume root; = gVolRoot[0] */

/* ---- multi-volume / mount support ----
 * At boot we enumerate EVERY SimpleFileSystem volume into gVolRoot[]; the boot
 * (ESP) volume is forced to index 0. Userland can mount any other volume under
 * an arbitrary directory via the `mount` syscall/command, and the path resolver
 * (fs_open_by_path) transparently routes opens whose path has a mount-point
 * prefix to the right volume. */
#define MAX_VOLUMES 8
#define MAX_MOUNTS  8
static EFI_FILE_PROTOCOL *gVolRoot[MAX_VOLUMES];      /* root dir of each volume */
static EFI_BLOCK_IO_PROTOCOL *gVolBlk[MAX_VOLUMES];   /* raw block device per volume (for format) */
static int              gVolCount = 0;

/* ---- whole-disk block devices (for partition management) ----
 * Enumerated separately from volumes: a volume (SimpleFileSystem) is a
 * partition, while a whole disk (LogicalPartition==0) is the raw device we
 * write MBR/GPT to. gDisk[0] is treated as the system (boot) disk and is
 * protected from partition operations. */
#define MAX_DISKS 8
static EFI_BLOCK_IO_PROTOCOL *gDiskBlk[MAX_DISKS];
static UINT64 gDiskTotal[MAX_DISKS];   /* total sectors */
static UINT32 gDiskBlock[MAX_DISKS];   /* block size in bytes */
static int    gDiskCount = 0;
static char             gMountPoint[MAX_MOUNTS][64];  /* absolute mount dirs */
static int              gMountVol[MAX_MOUNTS];        /* volume index per mount */
static int              gMountCount = 0;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *gConIn = NULL;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *gConInEx = NULL;   /* Ex protocol (modifier state); may be NULL */

/* graphics (GOP framebuffer) + mouse (absolute pointer) */
static EFI_GRAPHICS_OUTPUT_PROTOCOL   *gGop = NULL;
static EFI_ABSOLUTE_POINTER_PROTOCOL  *gAbs = NULL;
static UINT64 gFbBase = 0;
static UINT32 gFbW = 0, gFbH = 0, gFbStride = 0, gFbFormat = 0;
/* For GOP devices that expose NO linear CPU framebuffer (Blt-only, e.g.
 * virtio-gpu-pci), we keep a shadow framebuffer in kernel memory and Blt it
 * to the screen on each input/sleep syscall. The whole flat OS shares one
 * address space, so the shadow is just normal kernel memory. */
static UINT8  *gFbShadow = NULL;
static int     gFbBltOnly = 0;
static UINTN   gFbShadowSize = 0;
static UINT32  gFbDirty = 0;       /* shadow changed since last present */

/* ---- framebuffer text console (kcon) ---- */
#define KCELLW 16
#define KCELLH 32
static int   g_kinit = 0;          /* kcon dimensions resolved */
static int   g_kcon_enable = 0;    /* shell owns the framebuffer (draw to it) */
static int   g_kcols = 0, g_krows = 0;
static UINT16 g_kch[64][200];
static UINT8  g_kat[64][200];
static UINT8  g_kwide[64][200];    /* 0 normal, 1 CJK lead, 2 CJK trail */
static int   g_kcx = 0, g_kcy = 0;
static int   g_kfg = 7, g_kbg = 0;
/* forward declarations (implemented after gfx/gop primitives) */
static void kcon_init(void);
static void kcon_clear(void);
static void kcon_present(void);
static void kcon_putc(UINT32 cp);

/* clang/LLVM stack-probe stub. The freestanding build links no compiler-rt, yet
 * a large stack frame in sys_dispatch makes clang emit a __chkstk call. The
 * firmware already hands us one flat, valid stack region, so probing is
 * unnecessary; an empty stub satisfies the linker without changing behavior. */
void __chkstk(void) { /* intentionally empty */ }
static int   gMousePrev = 0;       /* previous LEFT  button state (for click-edge) */
static int   gMousePrevR = 0;      /* previous RIGHT button state (for click-edge) */

/* current working directory, ASCII, Unix-style slash-separated, e.g. "/" or "/foo" */
static char gCwd[256] = "/";

/* current terminal text colors (EFI color index 0..15); bg default black */
static int g_term_fg = 7;   /* 7 = light gray (UEFI default foreground) */
static int g_term_bg = 0;   /* 0 = black */

/* ===================================================================
 * Console output (ConOut -> QEMU -serial)
 * =================================================================== */
static void con_print(const CHAR16 *s) {
    if (gST && gST->ConOut && gST->ConOut->OutputString)
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
    if (s) { const CHAR16 *p = s; while (*p) kcon_putc((UINT32)*p++); }
    kcon_present();
}

/* ---- EFI text-output members not fully described in efi.h ---- */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
    UINTN *Columns, UINTN *Rows);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
typedef struct {
    INT32  MaxMode;
    INT32  Mode;
    INT32  Attribute;
    INT32  CursorColumn;
    INT32  CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

/* Print ASCII bytes to the console, mapping '\n' -> CRLF. */
static void con_write_bytes(const char *buf, int n) {
    /* serial (best-effort, byte-oriented, '\n' -> CRLF) */
    CHAR16 line[160];
    int i = 0;
    for (int k = 0; k < n; k++) {
        unsigned char ch = (unsigned char)buf[k];
        if (ch == '\n') { line[i++] = (CHAR16)'\r'; line[i++] = (CHAR16)'\n'; }
        else if (ch == '\r') { /* ignore */ }
        else if (ch == 0) { break; }
        else line[i++] = (CHAR16)ch;
        if (i >= 150) { line[i] = 0; if (gST && gST->ConOut) gST->ConOut->OutputString(gST->ConOut, line); i = 0; }
    }
    if (i > 0) { line[i] = 0; if (gST && gST->ConOut) gST->ConOut->OutputString(gST->ConOut, line); }
    /* framebuffer console: decode UTF-8 -> codepoints -> kcon (no serial coupling).
     * NOTE: iterate over the byte pointer, NOT over a byte count k<n. A multi-byte
     * sequence advances p by len (2 or 3), so a k<n loop would walk past the end of
     * buf and feed stray stack bytes to kcon as garbage glyphs (the "乱码" bug). */
    const unsigned char *p = (const unsigned char *)buf;
    const unsigned char *end = p + n;
    while (p < end) {
        UINT32 cp; int len = 1;
        if (*p < 0x80) { cp = *p; }
        else if ((*p & 0xE0) == 0xC0 && p + 1 < end && (p[1] & 0xC0) == 0x80) {
            cp = ((UINT32)(*p & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((UINT32)(*p & 0x0F) << 12) | ((UINT32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3;
        } else { cp = *p; }   /* invalid lead byte: draw its low byte as a fallback box */
        p += len;
        kcon_putc(cp);
    }
    kcon_present();
}

static void con_print_ascii(const char *s) {
    int n = 0; while (s[n]) n++;
    con_write_bytes(s, n);
}

static void con_print_dec(UINT64 v) {
    CHAR16 rev[24], out[24];
    INTN i = 0, j;
    if (v == 0) { con_print(L"0"); return; }
    while (v) { rev[i++] = (CHAR16)('0' + (v % 10)); v /= 10; }
    for (j = 0; j < i; j++) out[j] = rev[i - 1 - j];
    out[i] = 0;
    con_print(out);
}

/* Apply the current terminal foreground/background color to ConOut.
   EFI text attribute encodes as (background << 4) | (foreground & 0x0F). */
static void term_set_attr(void) {
    if (gST && gST->ConOut && gST->ConOut->SetAttribute) {
        EFI_TEXT_SET_ATTRIBUTE sa = (EFI_TEXT_SET_ATTRIBUTE)gST->ConOut->SetAttribute;
        sa(gST->ConOut, (UINTN)(((g_term_bg & 0x0F) << 4) | (g_term_fg & 0x0F)));
    }
}

/* Read up to n bytes from the keyboard into buf, one line (ends on Enter).
 * Returns bytes stored (includes trailing '\n'). Blocking. */
static int con_read_bytes(char *buf, int n) {
    int got = 0;
    EFI_INPUT_KEY key;
    if (!gConIn) return 0;
    for (;;) {
        EFI_STATUS st = gConIn->ReadKeyStroke(gConIn, &key);
        if (EFI_ERROR(st)) { gBS->Stall(5000); continue; }  /* no key yet: keep waiting for a full line */
        UINT16 c = key.UnicodeChar;
        if (c == 0x0D) {                  /* Enter */
            buf[got++] = '\n';
            con_write_bytes("\n", 1);
            break;
        } else if (c == 0x08) {           /* Backspace */
            if (got > 0) { got--; con_write_bytes("\b", 1); }
        } else if (c >= 0x20 && c < 0x7f) {
            buf[got++] = (char)c;
            con_write_bytes((char *)&c, 1);
        }
        if (got >= n - 1) break;
    }
    return got;
}

/* ===================================================================
 * String helpers (ASCII)
 * =================================================================== */
static int  my_strlen(const char *s){ int n=0; while(s[n]) n++; return n; }
static int  my_strcmp(const char *a, const char *b){
    while(*a && *a==*b){a++;b++;}
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void my_strcpy(char *d, const char *s){ while(*s){*d++=*s++;} *d=0; }
static void my_strcat(char *d, const char *s){ while(*d) d++; while(*s){*d++=*s++;} *d=0; }
static int  a2c(const char *s, CHAR16 *d, int max){
    int i=0; while(s[i] && i<max-1){ d[i]=(CHAR16)(unsigned char)s[i]; i++; }
    d[i]=0; return i;
}
/* ASCII path -> EFI CHAR16 path; map '/' to '\\' (UEFI FAT requires backslash). */
static void a2c_e(const char *s, CHAR16 *d, int max){
    int i=0; while(s[i] && i<max-1){ char c=s[i]; d[i]=(c=='/')?(CHAR16)'\\':(CHAR16)(unsigned char)c; i++; }
    d[i]=0;
}

/* Join cwd + name into out (ASCII, Unix style). cwd "/" means root. */
static int build_argv(char *rest, char ***argvp, VOID **poolp);  /* fwd decl (defined near cmd_open) */
static void join_path(const char *cwd, const char *name, char *out){
    if (name[0] == '/') name++;   /* tolerate absolute path passed as the name */
    if (cwd[1] == 0) {            /* root */
        out[0] = '/'; out[1] = 0;
        my_strcat(out, name);
    } else {
        my_strcpy(out, cwd);
        my_strcat(out, "/");
        my_strcat(out, name);
    }
}

/* ===================================================================
 * Filesystem: locate ESP root + helpers
 * =================================================================== */
#define EFI_FILE_DIRECTORY_ATTR  0x10ULL
#define EFI_FILE_INFO_ID \
    {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}

typedef struct {
    UINT64 Size;
    UINT64 FileOffset;
    UINT64 FileSize;
    UINT8  _time[48];        /* 3 x EFI_TIME placeholder (matches EDK2 layout) */
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

/* Enumerate ALL SimpleFileSystem volumes into gVolRoot[] (boot/ESP forced to
 * index 0) so that external storage can be mounted later. */
static int find_root(void) {
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN cnt = 0; EFI_HANDLE *hs = NULL;
    EFI_STATUS st = gBS->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &cnt, &hs);
    if (EFI_ERROR(st) || cnt == 0 || !hs) return -1;

    gVolCount = 0;
    int efiAt0 = 0;
    CHAR16 efi[] = {'\\','E','F','I',0};
    for (UINTN i = 0; i < cnt; i++) {
        if (gVolCount >= MAX_VOLUMES) break;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *cand = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &fsGuid, (VOID **)&cand))) continue;
        EFI_FILE_PROTOCOL *cr = NULL;
        if (EFI_ERROR(cand->OpenVolume(cand, &cr))) continue;
        int idx = gVolCount++;
        gVolRoot[idx] = cr;
        /* also try to grab the raw block device behind this volume (used by format).
         * For a whole-disk FAT volume the BlockIO lives on the same handle; for a
         * partitioned disk it may be on a parent handle (unavailable here) -> left NULL. */
        {
            EFI_BLOCK_IO_PROTOCOL *blk = NULL;
            EFI_GUID blkGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
            if (!EFI_ERROR(gBS->HandleProtocol(hs[i], &blkGuid, (VOID **)&blk))) gVolBlk[idx] = blk;
            else gVolBlk[idx] = NULL;
        }
        if (idx == 0) {
            EFI_FILE_PROTOCOL *t = NULL;
            if (!EFI_ERROR(cr->Open(cr, &t, efi, EFI_FILE_MODE_READ, 0))) {
                t->Close(t); efiAt0 = 1;
            }
        }
    }
    gBS->FreePool(hs);
    if (gVolCount == 0) return -1;
    /* Prefer the first enumerated handle (the boot device) as "/". Only if it
     * lacks an \EFI directory do we promote the first EFI volume to index 0. */
    if (!efiAt0) {
        for (int i = 1; i < gVolCount; i++) {
            EFI_FILE_PROTOCOL *t = NULL;
            if (!EFI_ERROR(gVolRoot[i]->Open(gVolRoot[i], &t, efi, EFI_FILE_MODE_READ, 0))) {
                t->Close(t);
                EFI_FILE_PROTOCOL *tmp = gVolRoot[0];
                gVolRoot[0] = gVolRoot[i];
                gVolRoot[i] = tmp;
                /* keep the raw block device in lockstep with the root dir, or the
                 * formatter would write to the wrong volume (desync = data loss). */
                EFI_BLOCK_IO_PROTOCOL *btmp = gVolBlk[0];
                gVolBlk[0] = gVolBlk[i];
                gVolBlk[i] = btmp;
                break;
            }
        }
    }
    gRoot = gVolRoot[0];
    return 0;
}

/* ---- whole-disk / partition management ---- */
#define MBR_PART_OFF 446   /* partition entries start at offset 446 in LBA0 */
/* forward decl: FAT formatter (vol_format_fat_ex) is defined further below */
static int vol_format_fat_ex(EFI_BLOCK_IO_PROTOCOL *blk, UINT64 part_start, UINT64 part_size, const char *label);

/* layout MUST match userland disk_info_t / part_info_t (see syscall.h) */
typedef struct { UINT64 total; UINT32 block; UINT8 is_sys; UINT8 _pad[3]; } k_disk_info;
typedef struct { UINT64 start; UINT64 size; UINT32 type; UINT8 active; UINT8 used; UINT8 _pad[2]; } k_part_info;

/* Enumerate whole-disk block devices (LogicalPartition==0). Called once at boot
 * after find_root(). gDisk[0] is the boot/system disk and is protected. */
static void disk_enum(void) {
    EFI_GUID blkGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN cnt = 0; EFI_HANDLE *hs = NULL;
    EFI_STATUS st = gBS->LocateHandleBuffer(ByProtocol, &blkGuid, NULL, &cnt, &hs);
    gDiskCount = 0;
    if (EFI_ERROR(st) || !hs) return;
    for (UINTN i = 0; i < cnt && gDiskCount < MAX_DISKS; i++) {
        EFI_BLOCK_IO_PROTOCOL *blk = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &blkGuid, (VOID **)&blk))) continue;
        if (!blk || !blk->Media) continue;
        if (blk->Media->LogicalPartition) continue;   /* skip partition-level children */
        if (blk->Media->ReadOnly) continue;
        gDiskBlk[gDiskCount]   = blk;
        gDiskBlock[gDiskCount] = (blk->Media->BlockSize) ? (UINT32)blk->Media->BlockSize : 512;
        gDiskTotal[gDiskCount] = (UINT64)blk->Media->LastBlock + 1;
        gDiskCount++;
    }
    gBS->FreePool(hs);
}

static int disk_read_mbr(int disk, UINT8 *mbr) {
    if (disk < 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
    UINTN bs = gDiskBlock[disk] ? gDiskBlock[disk] : 512;
    EFI_STATUS st = gDiskBlk[disk]->ReadBlocks(gDiskBlk[disk], gDiskBlk[disk]->Media->MediaId, 0, bs, mbr);
    return EFI_ERROR(st) ? -1 : 0;
}
static int disk_write_mbr(int disk, UINT8 *mbr) {
    if (disk < 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
    UINTN bs = gDiskBlock[disk] ? gDiskBlock[disk] : 512;
    EFI_STATUS st = gDiskBlk[disk]->WriteBlocks(gDiskBlk[disk], gDiskBlk[disk]->Media->MediaId, 0, bs, mbr);
    if (EFI_ERROR(st)) return -1;
    gDiskBlk[disk]->FlushBlocks(gDiskBlk[disk]);
    return 0;
}
static UINT32 rd32le(const UINT8 *p){ return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24); }
static void   wr32le(UINT8 *p, UINT32 v){ p[0]=(UINT8)(v&0xFF); p[1]=(UINT8)((v>>8)&0xFF); p[2]=(UINT8)((v>>16)&0xFF); p[3]=(UINT8)((v>>24)&0xFF); }

/* Create a new primary MBR partition on disk (disk 0 = system, protected),
 * filling the unallocated region (aligned to 1 MiB). Auto-formats it as FAT. */
static int disk_create_part(int disk, UINT64 req, UINT32 ptype) {
    if (disk <= 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
    if (gDiskBlk[disk]->Media->ReadOnly) return -3;
    if (ptype == 0) ptype = 0x0C;   /* FAT32 LBA by default */
    UINT8 mbr[512]; if (disk_read_mbr(disk, mbr)) return -2;
    int slot = -1;
    for (int p = 0; p < 4; p++) if (mbr[MBR_PART_OFF + p*16 + 4] == 0) { slot = p; break; }
    if (slot < 0) return -9;        /* no free primary slot */
    UINT64 end = 2048;              /* first usable LBA (1 MiB) */
    for (int p = 0; p < 4; p++) {
        if (mbr[MBR_PART_OFF + p*16 + 4] == 0) continue;
        UINT64 s = rd32le(mbr + MBR_PART_OFF + p*16 + 8);
        UINT64 z = rd32le(mbr + MBR_PART_OFF + p*16 + 12);
        if (s + z > end) end = s + z;
    }
    UINT64 start = (end + 2047) & ~((UINT64)2047);   /* align up to 1 MiB */
    UINT64 disk_sectors = gDiskTotal[disk];
    UINT64 avail = (disk_sectors > start) ? (disk_sectors - start) : 0;
    UINT64 size = req ? req : avail;
    if (size > avail) size = avail;
    if (size < 1) return -8;        /* no space */
    UINT8 *pe = mbr + MBR_PART_OFF + slot*16;
    pe[0] = 0; pe[1] = 0; pe[2] = 0; pe[3] = 0;
    pe[4] = (UINT8)ptype; pe[5] = 0; pe[6] = 0; pe[7] = 0;
    wr32le(pe + 8, (UINT32)start); wr32le(pe + 12, (UINT32)size);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (disk_write_mbr(disk, mbr)) return -2;
    if (vol_format_fat_ex(gDiskBlk[disk], start, size, "HUDOS") != 0) return -6;
    find_root();
    return slot;
}
static int disk_delete_part(int disk, int p) {
    if (disk <= 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
    if (p < 0 || p >= 4) return -2;
    UINT8 mbr[512]; if (disk_read_mbr(disk, mbr)) return -3;
    if (mbr[MBR_PART_OFF + p*16 + 4] == 0) return -4;   /* already empty */
    for (int i = 0; i < 16; i++) mbr[MBR_PART_OFF + p*16 + i] = 0;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (disk_write_mbr(disk, mbr)) return -3;
    find_root();
    return 0;
}
static int disk_format_part(int disk, int p, const char *label) {
    if (disk <= 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
    if (p < 0 || p >= 4) return -2;
    UINT8 mbr[512]; if (disk_read_mbr(disk, mbr)) return -3;
    if (mbr[MBR_PART_OFF + p*16 + 4] == 0) return -4;
    UINT64 start = rd32le(mbr + MBR_PART_OFF + p*16 + 8);
    UINT64 size  = rd32le(mbr + MBR_PART_OFF + p*16 + 12);
    if (vol_format_fat_ex(gDiskBlk[disk], start, size, label) != 0) return -6;
    find_root();
    return 0;
}

/* ---- volume / mount helpers ---- */

/* Compare up to n bytes of two ASCII strings. */
static int my_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
/* ASCII decimal parse (non-negative). */
static int atoi_l(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* Resolve an absolute ASCII path to (volume index, volume-relative path).
 * The volume-relative path has NO leading slash. */
static int fs_resolve(const char *apath, char *rel, int relcap) {
    int best = 0, best_len = 0;        /* default: boot volume (index 0) */
    for (int i = 0; i < gMountCount; i++) {
        const char *mp = gMountPoint[i];
        int mplen = my_strlen(mp);
        if (my_strcmp(apath, mp) == 0) { best = gMountVol[i]; best_len = mplen; break; }
        if (my_strncmp(apath, mp, mplen) == 0 && apath[mplen] == '/') {
            if (mplen > best_len) { best = gMountVol[i]; best_len = mplen; }
        }
    }
    int s = (apath[0] == '/') ? 1 : 0;  /* skip leading slash */
    s += best_len;                       /* skip the mount-point portion */
    if (apath[s] == '/') s++;            /* skip the slash after the mount point */
    int ri = 0;
    while (apath[s] && ri < relcap - 1) rel[ri++] = apath[s++];
    rel[ri] = 0;
    return best;
}

/* Open a file/dir given an absolute ASCII path. Routes to the correct volume
 * based on the mount table. Returns 0 on success, -1 on error. */
static int fs_open_by_path(const char *apath, UINT64 mode, UINT64 attr, EFI_FILE_PROTOCOL **out) {
    char rel[300];
    int vol = fs_resolve(apath, rel, (int)sizeof(rel));
    if (vol < 0 || vol >= gVolCount || !gVolRoot[vol]) return -1;
    CHAR16 w[300];
    if (rel[0] == 0) { w[0] = (CHAR16)'\\'; w[1] = 0; }   /* volume root */
    else a2c_e(rel, w, 300);
    EFI_STATUS st = gVolRoot[vol]->Open(gVolRoot[vol], out, w, mode, attr);
    return EFI_ERROR(st) ? -1 : 0;
}

/* Create a directory tree (and parents) on the BOOT volume. Used to materialise
 * mount points. apath is absolute, e.g. "/mnt/ext". The path is accumulated so
 * /mnt is created first, then /mnt/ext *inside* it (nested, not sibling). */
static void fs_mkdir_p(const char *apath) {
    char path[256]; int pi = 0;
    path[pi++] = '/';                       /* root-relative prefix */
    for (int i = 1; apath[i] && pi < 255; i++) {
        char c = apath[i];
        if (c == '/') {
            if (pi > 1) {                   /* flush the accumulated component */
                path[pi] = 0;
                CHAR16 w[300]; a2c_e(path, w, 300);
                EFI_FILE_PROTOCOL *d = NULL;
                if (!EFI_ERROR(gVolRoot[0]->Open(gVolRoot[0], &d, w,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        EFI_FILE_DIRECTORY_ATTR))) d->Close(d);
            }
            if (pi > 1 && path[pi - 1] != '/') path[pi++] = '/';
        } else {
            path[pi++] = c;
        }
    }
    if (pi > 1) {                           /* trailing component (no final slash) */
        path[pi] = 0;
        CHAR16 w[300]; a2c_e(path, w, 300);
        EFI_FILE_PROTOCOL *d = NULL;
        if (!EFI_ERROR(gVolRoot[0]->Open(gVolRoot[0], &d, w,
                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                EFI_FILE_DIRECTORY_ATTR))) d->Close(d);
    }
}

/* Volume-aware mkdir for the user-facing syscall (433): create `apath` (and any
 * missing parents) on whichever volume owns it per the mount table. This is what
 * lets a "File Manager" create folders on a mounted external volume. The kernel's
 * own mount-point materialisation keeps using fs_mkdir_p, which is deliberately
 * hard-wired to the boot volume (the mount point entry must live there). */
static void fs_mkdir_vfs(const char *apath) {
    char rel[256];
    int vol = fs_resolve(apath, rel, (int)sizeof(rel));
    if (vol < 0 || vol >= gVolCount || !gVolRoot[vol]) return;
    char path[256]; int pi = 0;
    path[pi++] = '/';                        /* volume-relative root prefix */
    for (int i = 0; rel[i] && pi < 255; i++) {
        char c = rel[i];
        if (c == '/') {
            if (pi > 1) {                    /* flush the accumulated component */
                path[pi] = 0;
                CHAR16 w[300]; a2c_e(path, w, 300);
                EFI_FILE_PROTOCOL *d = NULL;
                if (!EFI_ERROR(gVolRoot[vol]->Open(gVolRoot[vol], &d, w,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        EFI_FILE_DIRECTORY_ATTR))) d->Close(d);
            }
            if (pi > 1 && path[pi - 1] != '/') path[pi++] = '/';
        } else {
            path[pi++] = c;
        }
    }
    if (pi > 1) {                            /* trailing component (no final slash) */
        path[pi] = 0;
        CHAR16 w[300]; a2c_e(path, w, 300);
        EFI_FILE_PROTOCOL *d = NULL;
        if (!EFI_ERROR(gVolRoot[vol]->Open(gVolRoot[vol], &d, w,
                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                EFI_FILE_DIRECTORY_ATTR))) d->Close(d);
    }
}

/* Read a volume's label into out (ASCII). Falls back to "(nolabel)". */
static void vol_get_label(int idx, char *out, int cap) {
    out[0] = 0;
    if (idx < 0 || idx >= gVolCount || !gVolRoot[idx]) return;
    EFI_GUID infoGuid = EFI_FILE_SYSTEM_INFO_GUID;
    UINT8 ib[256]; UINTN isz = sizeof(ib);
    if (EFI_ERROR(gVolRoot[idx]->GetInfo(gVolRoot[idx], &infoGuid, &isz, ib))) { my_strcpy(out, "(nolabel)"); return; }
    EFI_FILE_SYSTEM_INFO *fi = (EFI_FILE_SYSTEM_INFO *)ib;
    CHAR16 *lbl = fi->VolumeLabel;
    int i = 0; while (lbl[i] && i < cap - 1) { out[i] = (char)lbl[i]; i++; }
    out[i] = 0;
    if (i == 0) my_strcpy(out, "(nolabel)");
}

/* Append a NUL-terminated string to a fixed buffer with bounds checking. */
static void kbuf_append(char *buf, int *pos, int cap, const char *s) {
    while (*s && *pos < cap - 1) buf[(*pos)++] = *s++;
}

/* Render the volume + mount tables as text into buf. */
static void mount_list_text(char *buf, int cap) {
    int p = 0;
    kbuf_append(buf, &p, cap, "Volumes:\n");
    for (int i = 0; i < gVolCount; i++) {
        char lbl[32]; vol_get_label(i, lbl, 32);
        char line[128]; int li = 0;
        line[li++] = ' '; line[li++] = (char)('0' + i); line[li++] = ' '; line[li++] = ':'; line[li++] = ' ';
        for (int k = 0; lbl[k] && li < 120; k++) line[li++] = lbl[k];
        line[li++] = '\n'; line[li] = 0;
        kbuf_append(buf, &p, cap, line);
    }
    kbuf_append(buf, &p, cap, "Mounts:\n");
    if (gMountCount == 0) kbuf_append(buf, &p, cap, "  (none)\n");
    for (int i = 0; i < gMountCount; i++) {
        char line[128]; int li = 0;
        line[li++] = ' ';
        for (int k = 0; gMountPoint[i][k] && li < 100; k++) line[li++] = gMountPoint[i][k];
        line[li++] = ' '; line[li++] = '-'; line[li++] = '>'; line[li++] = ' ';
        line[li++] = 'v'; line[li++] = 'o'; line[li++] = 'l'; line[li++] = (char)('0' + gMountVol[i]);
        line[li++] = '\n'; line[li] = 0;
        kbuf_append(buf, &p, cap, line);
    }
    buf[p] = 0;
}

/* Mount volume `volspec` (index or label) at absolute directory `dir`.
 * Creates the mount directory on the boot volume if needed. Returns:
 *   0 ok, -1 bad args, -2 no such volume, -3 cannot mount boot,
 *  -4 already mounted, -5 table full, -6 dir create failed, -7 not a directory. */
static int mount_do(const char *volspec, const char *dir) {
    if (!volspec || !dir || dir[0] != '/') return -1;
    int vol = -1;
    if (volspec[0] >= '0' && volspec[0] <= '9') vol = atoi_l(volspec);
    else for (int i = 0; i < gVolCount; i++) {
        char lbl[32]; vol_get_label(i, lbl, 32);
        if (my_strcmp(lbl, volspec) == 0) { vol = i; break; }
    }
    if (vol < 0 || vol >= gVolCount || !gVolRoot[vol]) return -2;
    if (vol == 0) return -3;               /* boot volume is already at / */
    /* normalise the mount point (collapse //, strip trailing slash) */
    char mp[64]; int mi = 0;
    for (int i = 0; dir[i] && mi < 63; i++) { if (dir[i] == '/' && dir[i+1] == '/') continue; mp[mi++] = dir[i]; }
    while (mi > 1 && mp[mi-1] == '/') mi--;
    mp[mi] = 0;
    for (int i = 0; i < gMountCount; i++) if (my_strcmp(gMountPoint[i], mp) == 0) return -4;
    if (gMountCount >= MAX_MOUNTS) return -5;
    fs_mkdir_p(mp);
    EFI_FILE_PROTOCOL *t = NULL;
    if (fs_open_by_path(mp, EFI_FILE_MODE_READ, 0, &t) != 0) return -6;
    EFI_GUID infoId = EFI_FILE_INFO_ID;
    UINT8 ib[128]; UINTN isz = sizeof(ib); int isdir = 0;
    if (!EFI_ERROR(t->GetInfo(t, &infoId, &isz, ib)))
        if (((EFI_FILE_INFO *)ib)->Attribute & EFI_FILE_DIRECTORY_ATTR) isdir = 1;
    t->Close(t);
    if (!isdir) return -7;
    my_strcpy(gMountPoint[gMountCount], mp);
    gMountVol[gMountCount] = vol;
    gMountCount++;
    return 0;
}

/* Unmount the directory `dir`. Returns 0 ok, -1 bad args, -2 cannot unmount /, -3 not mounted. */
static int umount_do(const char *dir) {
    if (!dir || dir[0] != '/') return -1;
    char mp[64]; int mi = 0;
    for (int i = 0; dir[i] && mi < 63; i++) { if (dir[i] == '/' && dir[i+1] == '/') continue; mp[mi++] = dir[i]; }
    while (mi > 1 && mp[mi-1] == '/') mi--;
    mp[mi] = 0;
    if (my_strcmp(mp, "/") == 0) return -2;
    for (int i = 0; i < gMountCount; i++) {
        if (my_strcmp(gMountPoint[i], mp) == 0) {
            for (int j = i; j < gMountCount - 1; j++) { my_strcpy(gMountPoint[j], gMountPoint[j+1]); gMountVol[j] = gMountVol[j+1]; }
            gMountCount--;
            return 0;
        }
    }
    return -3;
}

/* ---- disk / volume info + format ---- */
typedef struct {
    UINT64 total;   /* total bytes          */
    UINT64 free;    /* free bytes           */
    UINT64 used;    /* used bytes           */
    UINT32 block;   /* bytes per block      */
    UINT8  is_sys;  /* 1 = system (boot) volume, not formattable */
    UINT8  can_fmt; /* 1 = raw block device present & not system => formattable */
    UINT8  _pad[2];
    char   label[16];
} vol_info_t;

static void fat_w16(UINT8 *p, UINTN off, UINT16 v){ p[off]=(UINT8)(v&0xff); p[off+1]=(UINT8)((v>>8)&0xff); }
static void fat_w32(UINT8 *p, UINTN off, UINT32 v){ p[off]=(UINT8)(v&0xff); p[off+1]=(UINT8)((v>>8)&0xff);
                                                     p[off+2]=(UINT8)((v>>16)&0xff); p[off+3]=(UINT8)((v>>24)&0xff); }

/* Format volume idx (1..gVolCount-1) as FAT16 with the given ASCII label.
 * Writes a minimal but valid FAT16 superblock + FAT copies + empty root dir via
 * the volume's raw BlockIO. Returns 0 on success, <0 on error. The system
 * volume (idx 0) is always rejected. */
/* Format a FAT16 filesystem covering [part_start, part_start+part_size) sectors
 * on the raw block device `blk`. part_start=0, part_size=whole => whole disk.
 * Does NOT touch mounts or re-enumerate volumes (the caller decides that). */
static int vol_format_fat_ex(EFI_BLOCK_IO_PROTOCOL *blk, UINT64 part_start, UINT64 part_size, const char *label) {
    if (!blk || !blk->Media) return -2;
    EFI_BLOCK_IO_MEDIA *m = blk->Media;
    if (m->ReadOnly) return -3;
    UINTN bs = m->BlockSize; if (bs == 0) bs = 512;
    if (bs > 4096) return -7;
    UINT64 total = part_size;
    if (total < 2) return -4;

    /* FAT16 geometry: pick sectors/cluster so the cluster count fits FAT16 (< 65525). */
    UINTN root_entries = 512;
    UINTN root_sectors = (root_entries * 32 + bs - 1) / bs;
    UINTN reserved = 1;
    UINTN spc = 64, fat_sectors = 0, clusters = 0;
    for (;;) {
        UINTN cl = (total - reserved - root_sectors) / spc;
        for (int it = 0; it < 4; it++) {
            fat_sectors = (cl + 2) * 2;                  /* FAT16: 2 bytes/entry */
            fat_sectors = (fat_sectors + bs - 1) / bs;
            cl = (total - reserved - 2 * fat_sectors - root_sectors) / spc;
        }
        clusters = cl;
        if (clusters >= 1 && clusters < 65525) break;
        if (spc == 1) break;
        spc /= 2;
    }
    fat_sectors = (clusters + 2) * 2;
    fat_sectors = (fat_sectors + bs - 1) / bs;

    VOID *buf = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)bs, &buf))) return -5;
    UINT8 *b = (UINT8 *)buf;
    EFI_STATUS st;

    /* 1) boot sector (BPB) */
    for (UINTN i = 0; i < bs; i++) b[i] = 0;
    b[0] = 0xEB; b[1] = 0x00; b[2] = 0x90;          /* jump (no boot code) */
    { const char *o = "MSDOS5.0"; for (int i = 0; i < 8; i++) b[3 + i] = (UINT8)o[i]; }
    fat_w16(b, 11, (UINT16)bs);
    b[13] = (UINT8)spc;                              /* sectors per cluster */
    fat_w16(b, 14, (UINT16)reserved);
    b[16] = 2;                                      /* number of FATs */
    fat_w16(b, 17, (UINT16)root_entries);
    if (total <= 65535) fat_w16(b, 19, (UINT16)total); else fat_w16(b, 19, 0);
    b[21] = 0xF8;                                   /* media descriptor */
    fat_w16(b, 22, (UINT16)fat_sectors);
    fat_w16(b, 24, 32);                             /* sectors/track (dummy) */
    fat_w16(b, 26, 64);                             /* heads (dummy) */
    fat_w32(b, 28, 0);                              /* hidden sectors */
    fat_w32(b, 32, (UINT32)total);                  /* total sectors (32-bit) */
    b[36] = 0x80; b[37] = 0; b[38] = 0x29;          /* drive / reserved / boot signature */
    fat_w32(b, 39, 0x12345678);                     /* volume serial */
    { char lab[12]; for (int i = 0; i < 11; i++) lab[i] = ' ';
      if (label) { int j = 0; while (label[j] && j < 11) { lab[j] = label[j]; j++; } }
      for (int i = 0; i < 11; i++) b[43 + i] = (UINT8)lab[i]; }
    { const char *f = "FAT16   "; for (int i = 0; i < 8; i++) b[54 + i] = (UINT8)f[i]; }
    b[510] = 0x55; b[511] = 0xAA;
    st = blk->WriteBlocks(blk, m->MediaId, part_start + 0, bs, b);
    if (EFI_ERROR(st)) { gBS->FreePool(buf); return -6; }

    /* 2) FAT copies (first two entries reserved; rest = free) */
    for (UINTN copy = 0; copy < 2; copy++) {
        UINTN fat_lba = reserved + copy * fat_sectors;
        for (UINTN s = 0; s < fat_sectors; s++) {
            for (UINTN i = 0; i < bs; i++) b[i] = 0;
            if (s == 0) { fat_w16(b, 0, 0xFFF8); fat_w16(b, 2, 0xFFFF); }
            st = blk->WriteBlocks(blk, m->MediaId, part_start + fat_lba + s, bs, b);
            if (EFI_ERROR(st)) { gBS->FreePool(buf); return -6; }
        }
    }

    /* 3) root directory (zeroed = empty) */
    for (UINTN i = 0; i < bs; i++) b[i] = 0;
    UINTN root_lba = reserved + 2 * fat_sectors;
    for (UINTN s = 0; s < root_sectors; s++) {
        st = blk->WriteBlocks(blk, m->MediaId, part_start + root_lba + s, bs, b);
        if (EFI_ERROR(st)) { gBS->FreePool(buf); return -6; }
    }
    blk->FlushBlocks(blk);
    gBS->FreePool(buf);
    return 0;   /* raw FAT written; caller re-enumerates / fixes mounts */
}

/* Format a non-system volume (index) as FAT16. Wrapper that drops any mount
 * pointing at it and re-enumerates volumes so the fresh FS is visible again. */
static int vol_format_fat16(int idx, const char *label) {
    if (idx <= 0 || idx >= gVolCount) return -1;
    EFI_BLOCK_IO_PROTOCOL *blk = gVolBlk[idx];
    if (!blk || !blk->Media) return -2;
    UINT64 total = (UINT64)blk->Media->LastBlock + 1;
    int r = vol_format_fat_ex(blk, 0, total, label);
    if (r != 0) return r;
    for (int i = 0; i < gMountCount; i++) {
        if (gMountVol[i] == idx) {
            for (int j = i; j < gMountCount - 1; j++) { my_strcpy(gMountPoint[j], gMountPoint[j+1]); gMountVol[j] = gMountVol[j+1]; }
            gMountCount--; i--;
        }
    }
    find_root();
    return 0;
}

/* Open a file/dir at (cwd + name) with the given mode. Returns 0 on success. */
static int open_rel(const char *name, UINT64 mode, UINT64 attr, EFI_FILE_PROTOCOL **out) {
    char full[300];
    join_path(gCwd, name, full);
    return fs_open_by_path(full, mode, attr, out);
}
/* ===================================================================
 * Shell commands
 * =================================================================== */
/* Map a color name to an EFI foreground color index (0..15). -1 if unknown. */
static int color_to_efi(const char *name) {
    if (my_strcmp(name, "red")    == 0) return 12; /* light red   */
    if (my_strcmp(name, "blue")   == 0) return 9;  /* light blue  */
    if (my_strcmp(name, "purple") == 0) return 13; /* light magenta */
    if (my_strcmp(name, "green")  == 0) return 10; /* light green */
    if (my_strcmp(name, "white")  == 0) return 15;
    if (my_strcmp(name, "yellow") == 0) return 14;
    if (my_strcmp(name, "orange") == 0) return 6;  /* brown = closest to orange */
    return -1;
}

static void cmd_color(const char *arg) {
    if (!arg || arg[0] == 0) {
        con_print_ascii("color: usage: color <red|blue|purple|green|white|yellow|orange>\r\n");
        con_print_ascii("       (use 'color list' to show options)\r\n");
        return;
    }
    if (my_strcmp(arg, "list") == 0 || my_strcmp(arg, "ls") == 0) {
        con_print_ascii("available colors: red blue purple green white yellow orange\r\n");
        return;
    }
    int e = color_to_efi(arg);
    if (e < 0) {
        con_print_ascii("color: unknown color '");
        con_print_ascii(arg);
        con_print_ascii("'\r\n  available: red blue purple green white yellow orange\r\n");
        return;
    }
    g_term_fg = e;
    g_kfg = e;
    term_set_attr();
    con_print_ascii("terminal color set to ");
    con_print_ascii(arg);
    con_print_ascii("\r\n");
}

static void cmd_help(void) {
    con_print_ascii(
        "hudos-server commands:\r\n"
        "  ls               list current directory\r\n"
        "  cd [dir]         change directory (.. , /abs , or name)\r\n"
        "  mkdir <name>     create a directory\r\n"
        "  del <name>       delete a file\r\n"
        "  copy <src> <dst> copy a file\r\n"
        "  rename <old> <new> rename a file\r\n"
        "  open <file> [a]  load & run a Unix executable (aarch64 ELF)\r\n"
        "  tdesktop         launch the graphical desktop (tdesktop.elf)\r\n"
        "  gui              launch the Windows 3.2-style Program Manager (gui.elf)\r\n"
        "  pwd              print working directory\r\n"
        "  cls              clear screen\r\n"
        "  color <c>        set terminal text color\r\n"
        "  time             show current date and time\r\n"
        "  sndtest          probe the sound card and play a 440 Hz test tone\r\n"
        "  chinese          show a Chinese (CJK) display test\r\n"
        "  fbcon            toggle the CJK-capable framebuffer console\r\n"
        "  shutdown         power off the machine\r\n"
        "  mount [vol dir]  list volumes / mount a volume at a directory\r\n"
        "  umount <dir>     unmount a directory\r\n"
        "  help             this help\r\n");
}

/* minimal EFI_TIME (only the fields GetTime fills that we need) */
typedef struct {
  unsigned short Year;
  unsigned char  Month, Day, Hour, Minute, Second, Pad1;
  unsigned int   Nanosecond;
  short          TimeZone;
  unsigned char  Daylight, Pad2;
} EFI_TIME;

/* append an unsigned decimal (zero-padded to `width`) into a string buffer */
static void time_put_uint(char *b, int *p, unsigned v, int width){
  char tmp[16]; int i = 0;
  if (v == 0) tmp[i++] = '0';
  while (v > 0 && i < 15){ tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i < width) tmp[i++] = '0';
  while (i > 0) b[(*p)++] = tmp[--i];
}

static void cmd_time(void) {
  EFI_TIME t;
  EFI_STATUS (*gt)(EFI_TIME *, void *) =
      (EFI_STATUS(*)(EFI_TIME *, void *))gRT->GetTime;
  if (!gt || gt(&t, 0) != 0) {
    con_print_ascii("time: RTC unavailable\r\n");
    return;
  }
  char buf[48]; int p = 0;
  time_put_uint(buf, &p, t.Year,   4); buf[p++] = '-';
  time_put_uint(buf, &p, t.Month,  2); buf[p++] = '-';
  time_put_uint(buf, &p, t.Day,    2); buf[p++] = ' ';
  time_put_uint(buf, &p, t.Hour,   2); buf[p++] = ':';
  time_put_uint(buf, &p, t.Minute, 2); buf[p++] = ':';
  time_put_uint(buf, &p, t.Second, 2);
  buf[p++] = '\r'; buf[p++] = '\n'; buf[p] = 0;
  con_print_ascii(buf);
}

/* fetch <url> — HTTP GET via the virtio-net driver, print body to console.
 * Useful for debugging networking and for the hudos AI assistant's host bridge
 * (e.g. fetch http://10.0.2.2:8080/ask?q=hello). */
static long sys_fetch(const char *url, char *buf, int cap);  /* fwd decl; defined later */
static int  nw_net_selftest(char *ob, int cap);              /* fwd decl; defined later */
static char g_fetch_buf[60000];
static void cmd_fetch(const char *arg) {
    if (!arg || !arg[0]) { con_print_ascii("fetch: usage: fetch <url>\r\n"); return; }
    con_print_ascii("fetch: GET "); con_print_ascii(arg); con_print_ascii("\r\n");
    long n = sys_fetch(arg, g_fetch_buf, (int)sizeof(g_fetch_buf) - 1);
    if (n < 0) { con_print_ascii("fetch: failed (no network / bad host)\r\n"); return; }
    if (n > (int)sizeof(g_fetch_buf) - 1) n = (int)sizeof(g_fetch_buf) - 1;
    g_fetch_buf[n] = 0;
    con_print_ascii("--- 8< ---\r\n");
    /* print body as ASCII lines (CRLF -> CR) */
    for (long i = 0; i < n; i++) {
        char c = g_fetch_buf[i];
        if (c == '\n') con_print_ascii("\r");
        else if (c >= 0x20 || c == '\t' || c == '\r') { char t[2] = {c, 0}; con_print_ascii(t); }
        else { con_print_ascii("."); }
    }
    con_print_ascii("\r\n--- >8 ---\r\n");
    char len[16]; int lp = 0; long v = n;
    if (v == 0) len[lp++] = '0'; else { char r[16]; int rn = 0; while (v) { r[rn++] = (char)('0' + (v % 10)); v /= 10; } while (rn--) len[lp++] = r[rn]; }
    len[lp] = 0;
    con_print_ascii("fetch: "); con_print_ascii(len); con_print_ascii(" bytes\r\n");
}

static void cmd_pwd(void) {
    con_print_ascii("PWD: ");
    con_print_ascii(gCwd);
    con_write_bytes("\n", 1);
}

static void cmd_cls(void) {
    if (gST->ConOut->ClearScreen) gST->ConOut->ClearScreen(gST->ConOut);
    else con_print_ascii("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
    term_set_attr();   /* ClearScreen may reset to default attribute */
    if (g_kinit && g_kcon_enable) kcon_clear();
}

static void cmd_shutdown(void) {
    con_print_ascii("Shutting down...\r\n");
    /* allow the message to be flushed before we reset the platform */
    if (gBS) gBS->Stall(200000);
    if (gRT && gRT->ResetSystem)
        gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    /* should not return; if it does, just print a note */
    con_print_ascii("shutdown: ResetSystem unavailable\r\n");
}

static void cmd_ls(void) {
    EFI_FILE_PROTOCOL *d = NULL;
    if (fs_open_by_path(gCwd, EFI_FILE_MODE_READ, 0, &d) != 0) {
        con_print_ascii("ls: cannot open directory\r\n"); return;
    }
    UINT8 rbuf[2048];
    for (;;) {
        UINTN rs = sizeof(rbuf);
        EFI_STATUS st = d->Read(d, &rs, rbuf);
        if (EFI_ERROR(st) || rs == 0) break;
        UINTN off = 0;
        while (off + sizeof(EFI_FILE_INFO) <= rs) {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)(rbuf + off);
            if (fi->Size == 0) break;
            /* skip "." and ".." */
            CHAR16 *nm = fi->FileName;
            if (nm[0] == '.' && nm[1] == 0) { off += (UINTN)fi->Size; continue; }
            if (nm[0] == '.' && nm[1] == '.' && nm[2] == 0) { off += (UINTN)fi->Size; continue; }
            int isdir = (fi->Attribute & EFI_FILE_DIRECTORY_ATTR) ? 1 : 0;
            /* print name (ASCII) */
            char name[256]; int li = 0;
            for (int k = 0; nm[k] && li < 255; k++) name[li++] = (char)nm[k];
            name[li] = 0;
            con_print_ascii("  ");
            con_print_ascii(name);
            if (isdir) {
                con_print_ascii("   <DIR>\r\n");
            } else {
                con_print_ascii("   ");
                con_print_dec(fi->FileSize);
                con_write_bytes("\n", 1);
            }
            off += (UINTN)fi->Size;
        }
    }
    d->Close(d);
}

static void cmd_cd(const char *arg) {
    if (!arg || arg[0] == 0) { gCwd[0] = '/'; gCwd[1] = 0; return; }
    if (my_strcmp(arg, ".") == 0) return;
    char cand[300];
    if (my_strcmp(arg, "..") == 0) {
        if (gCwd[1] == 0) return;               /* already root */
        int last = 0;
        for (int i = 0; gCwd[i]; i++) if (gCwd[i] == '/') last = i;
        if (last == 0) { gCwd[0] = '/'; gCwd[1] = 0; }
        else gCwd[last] = 0;
        return;
    }
    if (arg[0] == '/') my_strcpy(cand, arg);   /* absolute */
    else join_path(gCwd, arg, cand);

    if (cand[1] == 0) { my_strcpy(gCwd, "/"); return; }  /* '/' is always a directory */

    /* verify it is a directory */
    EFI_FILE_PROTOCOL *t = NULL;
    if (fs_open_by_path(cand, EFI_FILE_MODE_READ, 0, &t) != 0 || !t) {
        con_print_ascii("cd: "); con_print_ascii(arg); con_print_ascii(": no such directory\r\n"); return;
    }
    EFI_GUID infoId = EFI_FILE_INFO_ID;
    UINT8 ib[128]; UINTN isz = sizeof(ib);
    EFI_STATUS st = t->GetInfo(t, &infoId, &isz, ib);
    int ok = 0;
    if (!EFI_ERROR(st)) { EFI_FILE_INFO *fi = (EFI_FILE_INFO *)ib; if (fi->Attribute & EFI_FILE_DIRECTORY_ATTR) ok = 1; }
    t->Close(t);
    if (!ok) { con_print_ascii("cd: "); con_print_ascii(arg); con_print_ascii(": not a directory\r\n"); return; }
    my_strcpy(gCwd, cand);
}

static void cmd_mkdir(const char *name) {
    if (!name || name[0] == 0) { con_print_ascii("mkdir: missing name\r\n"); return; }
    EFI_FILE_PROTOCOL *nd = NULL;
    if (open_rel(name, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                 EFI_FILE_DIRECTORY_ATTR, &nd) == 0) {
        con_print_ascii("mkdir: created "); con_print_ascii(name); con_write_bytes("\n", 1);
        nd->Close(nd);
    } else {
        con_print_ascii("mkdir: cannot create "); con_print_ascii(name); con_write_bytes("\n", 1);
    }
}

static void cmd_del(const char *name) {
    if (!name || name[0] == 0) { con_print_ascii("del: missing name\r\n"); return; }
    EFI_FILE_PROTOCOL *f = NULL;
    if (open_rel(name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &f) == 0) {
        f->Delete(f);   /* Delete closes + removes */
        con_print_ascii("del: removed "); con_print_ascii(name); con_write_bytes("\n", 1);
    } else {
        con_print_ascii("del: cannot delete "); con_print_ascii(name); con_write_bytes("\n", 1);
    }
}

static void cmd_copy(const char *src, const char *dst) {
    if (!src || !dst || src[0] == 0 || dst[0] == 0) { con_print_ascii("copy: usage: copy <src> <dst>\r\n"); return; }
    EFI_FILE_PROTOCOL *sf = NULL;
    if (open_rel(src, EFI_FILE_MODE_READ, 0, &sf) != 0) {
        con_print_ascii("copy: cannot open source "); con_print_ascii(src); con_write_bytes("\n", 1); return;
    }
    EFI_FILE_PROTOCOL *df = NULL;
    if (open_rel(dst, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &df) != 0) {
        sf->Close(sf);
        con_print_ascii("copy: cannot create dest "); con_print_ascii(dst); con_write_bytes("\n", 1); return;
    }
    UINT8 buf[2048];
    UINT64 total = 0;
    for (;;) {
        UINTN rs = sizeof(buf);
        EFI_STATUS st = sf->Read(sf, &rs, buf);
        if (EFI_ERROR(st) || rs == 0) break;
        UINTN ws = rs;
        st = df->Write(df, &ws, buf);
        if (EFI_ERROR(st)) { con_print_ascii("copy: write error\r\n"); break; }
        total += rs;
    }
    sf->Close(sf); df->Flush(df); df->Close(df);
    con_print_ascii("copy: "); con_print_dec(total); con_print_ascii(" bytes\r\n");
}

/* Recursively copy the contents of an already-open source directory handle
 * (sd) into an already-open destination directory handle (dd).  Both must be
 * open directory handles.  Returns 0 on success (errors are logged but do not
 * abort the whole copy).  Used by the SYS_copy syscall to support folders. */
static int copy_dir_into(EFI_FILE_PROTOCOL *sd, EFI_FILE_PROTOCOL *dd) {
    UINT8 rbuf[1024];
    for (;;) {
        UINTN rs = sizeof(rbuf);
        EFI_STATUS st = sd->Read(sd, &rs, rbuf);
        if (EFI_ERROR(st) || rs == 0) break;
        UINTN off = 0;
        while (off + sizeof(EFI_FILE_INFO) <= rs) {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)(rbuf + off);
            if (fi->Size == 0) break;
            CHAR16 *nm = fi->FileName;
            if (nm[0] == '.' && nm[1] == 0) { off += (UINTN)fi->Size; continue; }
            if (nm[0] == '.' && nm[1] == '.' && nm[2] == 0) { off += (UINTN)fi->Size; continue; }
            int isdir = (fi->Attribute & EFI_FILE_DIRECTORY_ATTR) ? 1 : 0;
            CHAR16 cw[256]; int ci = 0;
            for (int k = 0; nm[k] && ci < 255; k++) cw[ci++] = nm[k];
            cw[ci] = 0;
            EFI_FILE_PROTOCOL *cf = NULL;
            if (EFI_ERROR(sd->Open(sd, &cf, cw, EFI_FILE_MODE_READ, 0))) { off += (UINTN)fi->Size; continue; }
            if (isdir) {
                EFI_FILE_PROTOCOL *nd = NULL;
                if (EFI_ERROR(dd->Open(dd, &nd, cw,
                                       EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                       EFI_FILE_DIRECTORY_ATTR))) {
                    cf->Close(cf); off += (UINTN)fi->Size; continue;
                }
                copy_dir_into(cf, nd);
                cf->Close(cf); nd->Close(nd);
            } else {
                EFI_FILE_PROTOCOL *df = NULL;
                if (EFI_ERROR(dd->Open(dd, &df, cw,
                                       EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                    cf->Close(cf); off += (UINTN)fi->Size; continue;
                }
                UINT8 buf[1024];
                for (;;) {
                    UINTN crs = sizeof(buf);
                    EFI_STATUS cst = cf->Read(cf, &crs, buf);
                    if (EFI_ERROR(cst) || crs == 0) break;
                    UINTN cws = crs;
                    cst = df->Write(df, &cws, buf);
                    if (EFI_ERROR(cst)) break;
                }
                df->Flush(df); df->Close(df); cf->Close(cf);
            }
            off += (UINTN)fi->Size;
        }
    }
    return 0;
}

/* EFI has no native rename: implement as copy content + delete original. */
static void cmd_rename(const char *src, const char *dst) {
    if (!src || !dst || src[0] == 0 || dst[0] == 0) {
        con_print_ascii("rename: usage: rename <old> <new>\r\n"); return;
    }
    /* source must exist */
    EFI_FILE_PROTOCOL *sf = NULL;
    if (open_rel(src, EFI_FILE_MODE_READ, 0, &sf) != 0) {
        con_print_ascii("rename: cannot open source "); con_print_ascii(src); con_write_bytes("\n", 1); return;
    }
    /* if destination already exists, remove it first to avoid trailing garbage */
    EFI_FILE_PROTOCOL *ex = NULL;
    if (open_rel(dst, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &ex) == 0) {
        ex->Delete(ex);   /* Delete closes + removes */
    }
    /* create destination */
    EFI_FILE_PROTOCOL *df = NULL;
    if (open_rel(dst, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &df) != 0) {
        sf->Close(sf);
        con_print_ascii("rename: cannot create dest "); con_print_ascii(dst); con_write_bytes("\n", 1); return;
    }
    UINT8 buf[2048];
    UINT64 total = 0;
    for (;;) {
        UINTN rs = sizeof(buf);
        EFI_STATUS st = sf->Read(sf, &rs, buf);
        if (EFI_ERROR(st) || rs == 0) break;
        UINTN ws = rs;
        st = df->Write(df, &ws, buf);
        if (EFI_ERROR(st)) { con_print_ascii("rename: write error\r\n"); break; }
        total += rs;
    }
    sf->Close(sf); df->Flush(df); df->Close(df);
    /* remove original */
    EFI_FILE_PROTOCOL *df2 = NULL;
    if (open_rel(src, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &df2) == 0) {
        df2->Delete(df2);   /* Delete closes + removes */
    }
    con_print_ascii("rename: "); con_print_ascii(src); con_print_ascii(" -> ");
    con_print_ascii(dst); con_print_ascii("  ("); con_print_dec(total);
    con_print_ascii(" bytes)\r\n");
}

/* ===================================================================
 * ELF loader (aarch64 ELF64, PIE with RELATIVE relocations)
 * =================================================================== */
typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

typedef struct {
    UINT64 r_offset;
    UINT64 r_info;
    UINT64 r_addend;
} Elf64_Rela;

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define ET_EXEC    2
#define ET_DYN     3
#define EM_AARCH64 183
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_ABS64    257
#define DT_NULL   0
#define DT_RELA   7
#define DT_RELASZ 8
#define DT_RELAENT 9

/* Load an ELF from memory. Returns 0 on success and fills out_*.
 * Loads at an arbitrary page, applies RELATIVE/ABS64 relocations. */
static int elf_load(const uint8_t *data, UINT64 size,
                    UINT64 *out_entry, UINT64 *out_base, UINT64 *out_pages, UINT64 *out_sp) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != 2) return -1;        /* ELFCLASS64 */
    if (eh->e_ident[5] != 1) return -1;        /* ELFDATA2LSB */
    if (eh->e_machine != EM_AARCH64) return -1;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return -1;
    if (eh->e_phnum == 0) return -1;

    /* size of loaded image */
    UINT64 maxaddr = 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff);
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            UINT64 end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end > maxaddr) maxaddr = end;
        }
    }
    if (maxaddr == 0) return -1;

    UINT64 img_pages  = (maxaddr + 0xFFF) / 4096;
    /* Userland stack: 16 KiB (4 pages) is far too small for media decoders such
     * as minimp3, whose mp3dec_decode_frame keeps ~13 KiB of float scratch on the
     * stack and overflows -> data abort ("Synchronous Exception"). 256 KiB is a
     * safe ceiling for any userland app on a 512 MiB machine. */
    UINT64 stack_pages = 64;
    UINT64 total_pages = img_pages + stack_pages;

    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_STATUS st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderCode, total_pages, &base);
    if (EFI_ERROR(st)) return -2;
    gBS->SetMem((VOID *)(UINTN)base, total_pages * 4096, 0);

    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_offset + ph[i].p_filesz > size) return -3;
            gBS->CopyMem((VOID *)(UINTN)(base + ph[i].p_vaddr),
                         (VOID *)(UINTN)(data + ph[i].p_offset), ph[i].p_filesz);
        }
    }

    /* dynamic relocations */
    UINT64 dt_rela = 0, dt_relasz = 0, dt_relaent = 24;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            const UINT64 *dyn = (const UINT64 *)(data + ph[i].p_offset);
            UINT64 n = ph[i].p_filesz / 16;
            for (UINT64 k = 0; k < n; k++) {
                UINT64 tag = dyn[k*2], val = dyn[k*2+1];
                if (tag == DT_NULL) break;
                if (tag == DT_RELA) dt_rela = val;
                else if (tag == DT_RELASZ) dt_relasz = val;
                else if (tag == DT_RELAENT) dt_relaent = val;
            }
        }
    }
    if (dt_relasz > 0 && dt_rela > 0) {
        const uint8_t *rela = (const uint8_t *)(UINTN)(base + dt_rela);
        UINT64 nrela = dt_relasz / dt_relaent;
        for (UINT64 k = 0; k < nrela; k++) {
            const uint8_t *e = rela + k * dt_relaent;
            UINT64 r_offset = *(const UINT64 *)(e + 0);
            UINT64 r_info   = *(const UINT64 *)(e + 8);
            INT64  r_addend = *(const INT64  *)(e + 16);
            UINT32 type = (UINT32)(r_info & 0xffffffff);
            if (type == R_AARCH64_RELATIVE) {
                *(UINT64 *)(UINTN)(base + r_offset) = base + (UINT64)r_addend;
            } else if (type == R_AARCH64_ABS64) {
                *(UINT64 *)(UINTN)(base + r_offset) = base + (UINT64)r_addend;
            }
            /* other reloc types are ignored (freestanding PIE only uses RELATIVE) */
        }
    }

    *out_entry = base + eh->e_entry;
    *out_base  = base;
    *out_pages = total_pages;
    *out_sp    = (base + img_pages * 4096 + stack_pages * 4096) & ~(UINT64)0xF;
    return 0;
}

/* Jump into the loaded ELF. x0=argc, x1=argv, x2=envp, x3=syscall ptr,
 * x4=userland stack pointer (the loader's own SP is NOT touched here;
 * the userland _start switches SP itself and restores it before returning).
 * Returns the value in x0 after the program returns (its exit code). */
static UINT64 call_elf(UINT64 entry, UINT64 argc, UINT64 argv, UINT64 envp,
                       UINT64 syscall_ptr, UINT64 sp) {
    /* The ELF entry returns its result (main's return value) in x0.
     * Bind rc to x0 so we capture it correctly after blr returns. */
    register UINT64 rc asm("x0");
    __asm__ volatile (
        "mov x4, %[usp]\n"
        "mov x0, %[a0]\n"
        "mov x1, %[a1]\n"
        "mov x2, %[a2]\n"
        "mov x3, %[a3]\n"
        "blr %[ent]\n"
        : "=r" (rc)
        : [usp] "r" (sp), [a0] "r" (argc), [a1] "r" (argv),
          [a2] "r" (envp), [a3] "r" (syscall_ptr), [ent] "r" (entry)
        : "x1","x2","x3","x4","lr","memory","cc"
    );
    return rc;
}

/* ===================================================================
 * Syscall shim (Linux aarch64 syscall numbers)
 * =================================================================== */
#define MAXFD 32
static EFI_FILE_PROTOCOL *g_fds[MAXFD];   /* indices 3..MAXFD-1 are files */
static int g_elf_exited = 0;
static long g_elf_rc = 0;

/* Terminal-exec capture: when a userland program is launched *inside* tdesktop's
 * terminal window (SYS_exec = 411), g_exec_out points at the caller's capture
 * buffer. While non-NULL, write(1)/write(2) are redirected into it (instead of
 * the serial console) and read(0) returns EOF. Reset to NULL when the child exits. */
static char  *g_exec_out = NULL;
static int    g_exec_out_cap = 0;
static int    g_exec_out_len = 0;

/* ---- interactive terminal: coroutine execution + stdin queue ----
 * A program launched from the Terminal runs as a RESUMABLE coroutine. When
 * it blocks on read(0) (no input available yet) it yields back to the GUI
 * (tdesktop) so the desktop can render captured output and feed keystrokes
 * into g_exec_in. This lets interactive programs (e.g. word.elf) read stdin.
 * State: 0 dead, 1 running (started, not yet resumed), 2 blocked, 3 done. */
typedef struct { UINT64 x[12]; UINT64 sp; } coctx_t;  /* x[0..10]=x19..x29, x[11]=x30(lr) */
static coctx_t g_co_parent;            /* desktop (tdesktop) context */
static coctx_t g_co_child;             /* launched-program context */
static int     g_co_state = 0;
/* When 1, a coroutine child launched by the gui program manager yields control
 * back to the parent (gui) on every gop_present (SYS_gop_present 442) so gui can
 * keep drawing its own chrome (e.g. a "返回" button) and detect a quit. Only the
 * gui sets this (via SYS_co_set_yield 428); tdesktop's children never set it, so
 * tdesktop's coroutine behavior is completely unaffected. */
static int     g_co_yield_on_input = 0;
static void   *g_co_child_img = NULL;  /* file image buffer (FreePool) */
static UINT64  g_co_child_base = 0, g_co_child_pages = 0;  /* loaded image (FreePages) */
static UINT64  g_co_child_stack = 0, g_co_child_entry = 0; /* child stack top / entry */
static int      g_co_child_argc = 0;   /* argv for the coroutine child (0 = none) */
static char   **g_co_child_argv = NULL;
static VOID    *g_co_argv_pool  = NULL;/* FreePool for the parsed argv (NULL if none) */
static char    g_exec_in[4096];        /* program stdin queue (bytes available to read) */
static int     g_exec_in_len = 0;      /* total bytes queued */
static int     g_exec_in_rd  = 0;      /* bytes already consumed by the child */

/* Switch from *from to *to, preserving callee-saved x19..x30 + sp.
 * 'to' resumes at the PC saved in to->x[11] (x30). Called from within a
 * syscall on whichever stack is currently active; never returns to its
 * caller (it returns into the 'to' context instead). */
static long sys_dispatch(long n, long a, long b, long c, long d, long e);  /* defined below */

/* ===================================================================
 * Phase B: EL0 user mode (privilege-level isolation)
 *
 * hudos never calls ExitBootServices, so UEFI boot services stay live for the
 * whole OS lifetime and they rely on the firmware's own VBAR_EL1 handlers
 * (the periodic timer drives USB polling, which is how the mouse updates).
 * A previous attempt installed a PERMANENT custom VBAR_EL1 and hung silently:
 * boot-service IRQs landed in a stub that just `eret`ed without acknowledging,
 * so the same IRQ re-fired forever.
 *
 * The design here is WINDOWED VBAR ownership:
 *   - VBAR_EL1 points at the firmware, exactly as today, except...
 *   - ...while a user program runs, when it points at hudos_el0_vectors and
 *     IRQ/FIQ are masked (there is no preemptive multitasking, and programs
 *     already reach the kernel every frame through syscalls).
 *   - The window closes the moment the program exits or faults.
 * Because IRQs are masked in the window, no EL0 IRQ path is needed, which is
 * precisely what avoided having to chain exceptions to the firmware.
 *
 * Scope reminder: this phase isolates PRIVILEGE, not address space. Page tables
 * are untouched, so a misbehaving program can still read kernel memory, but it
 * can no longer execute privileged instructions and can only enter the kernel
 * through SVC. Splitting the address space is a later phase.
 * =================================================================== */

/* trap frame pushed by the EL0 entry stub in mmu_asm.S: x0..x30 + 1 pad slot */
struct el0_frame { unsigned long long x[32]; };

/* Full EL0 execution context of the coroutine child. x[0..31] share the exact
 * layout of el0_frame (so a memcpy of x[] stays valid); elr/spsr/sp_el0 are the
 * extra special registers that hudos_el0_eret does NOT restore. Captured on a
 * "completed syscall" yield (gop_present / sleep) and restored on resume via
 * hudos_el0_ctx_eret, which erets straight back into EL0 with the WHOLE context
 * correct (not just x0..x30). This fixes the post-first-yield corruption crash
 * ("打开文件管理器即崩", SVC@EL1 control-flow corruption): the old resume reused
 * the first-trap frame's stale x0..x30 and never restored elr/spsr/sp_el0, so the
 * eret dropped into a bogus EL1 address and executed a stray svc. */
struct el0_ctx {
    unsigned long long x[32];     /* 0x00..0xFF  (== el0_frame) */
    unsigned long long elr;      /* 0x100 */
    unsigned long long spsr;      /* 0x108 */
    unsigned long long sp_el0;    /* 0x110 */
};
static struct el0_ctx g_co_child_ctx;
/* Pointer to the exception frame of the syscall currently being serviced; set by
 * hudos_el0_sync_handler, read by el0_co_yield to persist the child state. */
static struct el0_frame *g_el0_cur_frame = NULL;
/* 1 = last yield was a completed-syscall yield (resume via eret frame back into
 * EL0); 0 = blocking/kernel-resume yield (resume via co_switch into the handler,
 * e.g. a read() waiting for input). */
static int g_el0_resume_mode = 0;
static int g_el0_yield_pending = 0;  /* set by el0_co_yield(done=1) when a child
                                        yields via the ctx-eret path; the resume
                                        branch clears it and returns to the GUI
                                        instead of re-ereting into the child */

extern char hudos_el0_vectors[];
extern unsigned long long hudos_el0_stats[];
extern char hudos_el0_selftest_entry[];
extern void hudos_el0_eret(void *frame);
extern void hudos_el0_ctx_eret(void *ctx);
extern void hudos_el0_exit(long rc);
extern unsigned long long hudos_el0_enter(unsigned long long entry,
                                          unsigned long long usp,
                                          unsigned long long argc,
                                          unsigned long long argv,
                                          unsigned long long envp);

static unsigned long long g_fw_vbar = 0;  /* firmware VBAR_EL1, saved at init */
static int g_el0_quiet = 1;       /* 1 = no per-syscall tracing on UART */
static unsigned long long g_el0_vbar = 0; /* page-aligned copy of our vectors */
/* Saved EL0 SVC trap context. ELR_EL1/SPSR_EL1 are SHARED architectural registers:
 * whenever our SVC handler calls into firmware (e.g. Open()) the firmware may take
 * its own internal exception (disk I/O on QEMU routes through a firmware interrupt
 * handler that does msr daifclr/eret). That firmware exception OVERWRITES ELR_EL1/
 * SPSR_EL1 with the firmware's own live context and restores only the firmware value
 * on return — the child's SVC ELR/SPSR is destroyed. If we then eret with those
 * clobbered registers the CPU jumps to a firmware PC at EL1 and crashes ("EL1
 * EXCEPTION", ELR in child/firmware code, SPSR EL1h). So we snapshot ELR/SPSR at
 * handler entry and restore them before every use (the eret back to the child, and
 * el0_co_yield's context persist). */
static unsigned long long g_el0_saved_elr  = 0;
static unsigned long long g_el0_saved_spsr = 0;
static int g_el0_enable = 1;              /* run direct-exec children at EL0 */
static int g_el0_live = 0;                /* currently inside an EL0 window */

/* Phase B (TTBR0 private page table + ASID) support globals.
 * g_el0_fw_ttbr0 : the firmware's live TTBR0_EL1 (EL1 view we always keep for the
 *                  kernel / parent GUI). Captured at each child launch.
 * g_el0_child_ttbr0 : TTBR0 value to install while the child runs at EL0
 *                  (child ASID + cloned root). When Phase B is off it equals the
 *                  firmware TTBR0 so the asm switch is a harmless no-op.
 * g_el0_child_pt : PA of the cloned root table (for teardown).
 * g_el0_use_pt   : 1 = Phase B active, 0 = additive/EL1 fallback.
 * g_el0_phaseb   : master switch for Phase B. Default OFF (0) so the system boots
 *                  in the known-good additive scheme (which the user confirmed
 *                  works); set to 1 to exercise the private-page-table isolation.
 *                  A stale-ASID / TLB bug in Phase B is still under investigation,
 *                  so it stays opt-in until that is resolved. */
static unsigned long long g_el0_fw_ttbr0   = 0;
unsigned long long g_el0_child_ttbr0 = 0;   /* global: referenced by mmu_asm.S */
static unsigned long long g_el0_child_pt   = 0;
static int                 g_el0_use_pt    = 0;
static int                 g_el0_phaseb    = 0;   /* Phase B master switch (default OFF) */
/* EL0-accessible regions promoted in the cloned tree: the child image and the
 * argv pool (both must be readable/executable at EL0). */
static unsigned long long g_pt_cbase = 0, g_pt_cend = 0;
static unsigned long long g_pt_pbase = 0, g_pt_pend = 0;
/* Track every table page we allocate while cloning, so teardown can free them. */
#define EL0_PT_MAX 256
static unsigned long long g_el0_pt_pages[EL0_PT_MAX];
static int                 g_el0_pt_n = 0;
    /* Diagnostic load ranges (set at launch) so a crash can be mapped to a file. */
    static unsigned long long g_parent_base = 0, g_parent_end = 0; /* EL1 parent (gui) */
    static unsigned long long g_child_base  = 0, g_child_end  = 0; /* EL0 co child */
    /* Other EL0-granted / child-related buffers, for crash-site mapping. */
    static unsigned long long g_shadow_base = 0, g_shadow_end = 0;   /* FB shadow (granted) */
    static unsigned long long g_imgf_base   = 0, g_imgf_end   = 0;   /* raw ELF file buffer */
    static unsigned long long g_argp_base   = 0, g_argp_end   = 0;   /* argv pool */

static unsigned long long el0_mrs_esr(void) { unsigned long long v; __asm__ volatile("mrs %0, esr_el1" : "=r"(v)); return v; }
static unsigned long long el0_mrs_far(void) { unsigned long long v; __asm__ volatile("mrs %0, far_el1" : "=r"(v)); return v; }
static unsigned long long el0_mrs_elr(void) { unsigned long long v; __asm__ volatile("mrs %0, elr_el1" : "=r"(v)); return v; }
static unsigned long long el0_mrs_sp_el0(void) { unsigned long long v; __asm__ volatile("mrs %0, sp_el0" : "=r"(v)); return v; }
static unsigned long long el0_mrs_spsel(void) { unsigned long long v; __asm__ volatile("mrs %0, spsel" : "=r"(v)); return v; }
static unsigned long long el0_mrs_daif(void) { unsigned long long v; __asm__ volatile("mrs %0, daif" : "=r"(v)); return v; }
static void el0_set_vbar(unsigned long long v) {
    __asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(v) : "memory");
}

/* PL011 diagnostics. Deliberately not ConOut: this must work from a fault
 * context where the firmware's console services may be unusable. */
static void el0_putc(char c) {
    volatile unsigned int *fr = (volatile unsigned int *)(0x9000000ULL + 0x18);
    volatile unsigned int *dr = (volatile unsigned int *)(0x9000000ULL);
    for (int spin = 0; spin < 1000000; spin++) if (!(*fr & (1u << 5))) break;
    *dr = (unsigned int)(unsigned char)c;
}
static void el0_puts(const char *s) {
    while (*s) { char c = *s++; if (c == '\n') el0_putc('\r'); el0_putc(c); }
}
static void el0_puthex(const char *label, unsigned long long v) {
    el0_puts(label);
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)((v >> s) & 0xF);
        el0_putc((char)(d < 10 ? ('0' + d) : ('a' + d - 10)));
    }
    el0_putc('\n');
}

static void el0_init(void) {
    unsigned long long fw = 0;
    EFI_PHYSICAL_ADDRESS addr = 0;
    unsigned char *dst;
    unsigned i;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(fw));
    g_fw_vbar = fw;

    /* VBAR_EL1 must be 2KB aligned, and the ".align 11" in the asm only gives
     * SECTION-RELATIVE alignment, which the linker is free to discard — the
     * linked image placed the table somewhere that was not 2KB aligned, so the
     * CPU mis-routed exceptions to neighbouring slots (an SVC landed in the IRQ
     * vector). Rather than fight the linker we copy the table into a freshly
     * allocated page, which is guaranteed 4KB (hence 2KB) aligned. */
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData,
                                     1, &addr)))
        return;

    /* NOTE: we deliberately do NOT point VBAR at freshly allocated pages. This
     * firmware hands out AllocatePages addresses whose virtual mapping is not
     * usable for instruction fetch here (see the same caveat for the HDA
     * buffers elsewhere in this file), which turns the first exception into an
     * unrecoverable double fault. The table compiled into our own image is
     * already covered by executable, identity-mapped pages.
     *
     * VBAR_EL1 must be 2KB aligned. The ".align 11" in the asm is only
     * SECTION-relative and the linker may discard it, so verify at runtime:
     * an unaligned base makes the CPU mis-route exceptions into neighbouring
     * vector slots (an SVC landing in the IRQ vector). Better to disable EL0
     * and keep the firmware's vectors than to fail unpredictably. */
    {
        unsigned long long tbl = (unsigned long long)(unsigned long)hudos_el0_vectors;
        if (tbl & 0x7FFULL) {
            el0_puthex("el0: vector table misaligned, disabling:", tbl);
            g_el0_enable = 0;
            return;
        }
        g_el0_vbar = tbl;
    }
    (void)dst; (void)i;
}

/* Run a program at EL0 inside a VBAR window. Returns when the program exits. */
static long el0_run(unsigned long long entry, unsigned long long usp,
                    unsigned long long argc, unsigned long long argv,
                    unsigned long long envp) {
    unsigned long long daif;
    long rc;
    if (!g_fw_vbar || !g_el0_vbar) return -1;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #0x3" ::: "memory");   /* mask I(bit1) + F(bit0) */
    if (!g_el0_quiet) el0_puthex("el0: daif masked=", el0_mrs_daif());
    el0_set_vbar(g_el0_vbar);
    g_el0_live = 1;
    rc = (long)hudos_el0_enter(entry, usp, argc, argv, envp);
    g_el0_live = 0;
    el0_set_vbar(g_fw_vbar);
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return rc;
}

/* SVC entry from EL0: ESR EC==0x15 means "SVC instruction executed in AArch64".
 * Convention (same as Linux aarch64): number in x8, args in x0-x4, result in x0. */
/* forward refs: these are defined later in this file (the coroutine and
 * paging helpers are only reachable after their definitions) */
static void co_child_cleanup(void);
static void el0_ungrant_pages(void);
static int  el0_grant_pages(unsigned long long va, unsigned long long len);

/* ===================================================================
 * Address-space isolation (user-pointer range checking)
 *
 * A program at EL0 can only legally touch the pages we explicitly handed it
 * (its image, its stack, its argv page). Everything else stays AP=0b00, i.e.
 * EL1-only, so the kernel's memory is invisible to user code.
 *
 * The remaining hole is the CONFUSED DEPUTY: the kernel dereferences pointers
 * supplied by the user (write/read buffers, paths, output buffers). Without
 * checking, a user program could pass a KERNEL address and have the kernel read
 * or write it on its behalf. el0_ptr_ok()/el0_str_ok() close that hole by
 * requiring such pointers to lie inside the granted ranges. They only apply
 * while servicing a syscall that came from EL0 (g_el0_active) — the GUI and
 * other EL1 callers are unaffected.
 * =================================================================== */
#define EL0_RMAX 6
static unsigned long long g_el0_rstart[EL0_RMAX], g_el0_rlen[EL0_RMAX];
static int                g_el0_rn = 0;
static int                g_el0_active = 0;   /* servicing a syscall from EL0 */

static void el0_reset_ranges(void) { g_el0_rn = 0; }

static int el0_ptr_ok(unsigned long long va, unsigned long long len)
{
    int i;
    if (!len) return 1;
    if (!va) return 0;
    for (i = 0; i < g_el0_rn; i++)
        if (va >= g_el0_rstart[i] && va + len <= g_el0_rstart[i] + g_el0_rlen[i])
            return 1;
    return 0;
}

/* A user string is only accepted if it is NUL-terminated inside a granted
 * range, so the kernel can never be made to walk off into kernel memory. */
static int el0_str_ok(const char *str)
{
    unsigned long long va = (unsigned long long)(unsigned long)str;
    int i;
    if (!str) return 0;
    for (i = 0; i < g_el0_rn; i++) {
        unsigned long long en = g_el0_rstart[i] + g_el0_rlen[i];
        unsigned long long j;
        if (va < g_el0_rstart[i] || va >= en) continue;
        for (j = va; j < en; j++)
            if (*(const char *)(unsigned long)j == 0) return 1;
        return 0;
    }
    return 0;
}

#define EL0CHK(ptr, len) do { \
    if (g_el0_active && !el0_ptr_ok((unsigned long long)(unsigned long)(ptr), \
                                    (unsigned long long)(len))) return -1; \
} while (0)
#define EL0STR(str) do { \
    if (g_el0_active && !el0_str_ok(str)) return -1; \
} while (0)

/* ===================================================================
 * Coroutine children at EL0
 *
 * Two things make this different from the direct-exec (411) case:
 *  1. The GUI keeps running between slices of the child. While it does, the
 *     firmware's vectors are installed and it takes IRQs — which OVERWRITE
 *     ELR_EL1 and SPSR_EL1. So the child's EL0 state must be saved on yield and
 *     restored on resume, otherwise it would resume at a stale address.
 *  2. The child's syscalls must not run on the GUI's kernel stack. It gets its
 *     own kernel stack so a suspended child's frames cannot be clobbered.
 * =================================================================== */
void co_switch(coctx_t *from, coctx_t *to);      /* (also declared below) */
extern void hudos_el0_co_start(unsigned long long entry, unsigned long long usp,
                               unsigned long long ksp, unsigned long long argc,
                               unsigned long long argv, unsigned long long parent);

static int     g_el0_co       = 0;   /* the live coroutine child runs at EL0  */
static int     g_el0_co_first = 0;   /* its very first slice has not run yet  */
static UINT64  g_el0_co_ksp   = 0;   /* kernel stack for the EL0 child        */
static UINT64  g_el0_sv_sp_el0 = 0, g_el0_sv_elr = 0, g_el0_sv_spsr = 0;

static void el0_set_sp_el0(unsigned long long v) { __asm__ volatile("msr sp_el0, %0"  :: "r"(v)); }
static void el0_set_elr(unsigned long long v)    { __asm__ volatile("msr elr_el1, %0" :: "r"(v)); }
static void el0_set_spsr(unsigned long long v)   { __asm__ volatile("msr spsr_el1, %0" :: "r"(v)); }
static unsigned long long el0_mrs_spsr(void) { unsigned long long v; __asm__ volatile("mrs %0, spsr_el1" : "=r"(v)); return v; }

static void el0_save_child_state(void)
{
    g_el0_sv_sp_el0 = el0_mrs_sp_el0();
    g_el0_sv_elr    = el0_mrs_elr();
    g_el0_sv_spsr   = el0_mrs_spsr();
}
static void el0_restore_child_state(void)
{
    el0_set_sp_el0(g_el0_sv_sp_el0);
    el0_set_elr(g_el0_sv_elr);
    el0_set_spsr(g_el0_sv_spsr);
}

/* Yield from an EL0 child back to the GUI; replaces every
 * co_switch(&g_co_child, &g_co_parent) call site. */
static void el0_set_ttbr0(unsigned long long v);   /* fwd: defined after el0_build_child_pt */
static void el0_co_yield(int done)
{
    /* Only the LIVE EL0 coroutine child may yield here. The condition requires
     * g_el0_active, which hudos_el0_sync_handler sets to 1 *only* for an SVC
     * trapped from EL0 (and clears after the dispatch returns). The trusted EL1
     * GUI also reaches sys_dispatch directly (e.g. its gop_present calls
     * el0_co_yield), but there g_el0_active is 0 and g_el0_cur_frame is a STALE
     * pointer left over from the child's last trap. Acting on that stale frame
     * would capture a KERNEL address as the child context (the "second yield
     * elr=kernel, spsr=EL1" corruption) and make hudos_el0_ctx_eret eret into
     * kernel space -> Synchronous Exception. So for the EL1 GUI this is a clean
     * no-op: gop_present just returns and the GUI yields via co_resume(414). */
    if (!(g_el0_co && g_el0_active && g_el0_cur_frame))
        return;
    /* Restore the child's SVC trap context into ELR_EL1/SPSR_EL1 first: the firmware
     * call inside the current syscall may have clobbered them (see g_el0_saved_elr),
     * and we are about to persist elr/spsr from these registers. Without the restore
     * we would persist the firmware's leftover PC/mode and the resume would eret into
     * garbage. */
    __asm__ volatile("msr elr_el1,%0" :: "r"(g_el0_saved_elr) : "memory");
    __asm__ volatile("msr spsr_el1,%0" :: "r"(g_el0_saved_spsr) : "memory");
    /* Persist the FULL EL0 execution context from the current trap frame.
     * x0..x30 live only in this frame (pushed by hudos_el0_sync); elr/spsr/
     * sp_el0 are read live from the CPU. If we don't copy all of this out,
     * the next resume would reuse a stale frame and the child's registers /
     * return address / mode would be garbage after the first yield. */
    for (int i = 0; i < 32; i++) g_co_child_ctx.x[i] = g_el0_cur_frame->x[i];
    g_co_child_ctx.elr    = el0_mrs_elr();
    g_co_child_ctx.spsr   = el0_mrs_spsr();
        g_co_child_ctx.sp_el0 = el0_mrs_sp_el0();
        if (!g_el0_quiet) {
            el0_puts("[yield] child_ctx: ");
            el0_puthex("elr=", g_co_child_ctx.elr);
            el0_puthex("spsr=", g_co_child_ctx.spsr);
            el0_puthex("sp_el0=", g_co_child_ctx.sp_el0);
            el0_puthex("child_base=", g_child_base);
        }
        if (done) g_co_child_ctx.x[0] = 0;   /* completed syscall returns 0 */
    g_el0_resume_mode = done;
    if (done) g_el0_yield_pending = 1;   /* ctx-eret resume: next co_resume must
                                             return to the GUI after the child
                                             yields back, not re-eret */
    /* We are about to hand control to the PARENT (a trusted EL1 program such
     * as gui.elf). The child's syscall is no longer being serviced, so clear
     * g_el0_active: otherwise the parent's own syscalls (which also flow
     * through sys_dispatch) would be validated against the child's authorized
     * pointer ranges and rejected with -1, freezing the co-hosted GUI. */
    g_el0_active = 0;
    /* Phase B: while the trusted EL1 PARENT runs, restore the FULL firmware
     * page table (exactly the address space the additive scheme used, where
     * TTBR0 was always the firmware table). The child keeps its isolated
     * clone only while *it* executes at EL0. Running the GUI on the child's
     * demoted clone (all non-child pages EL1-only) perturbs firmware
     * boot-service / TLB state and stalls the co-hosted GUI ("卡"). */
    if (g_el0_use_pt) el0_set_ttbr0(g_el0_fw_ttbr0);
    el0_set_vbar(g_fw_vbar);
    __asm__ volatile("msr daifclr, #0x3" ::: "memory");   /* unmask I,F */
    /* Pop the EL0 trap frame off the child's kernel stack (same as
     * hudos_el0_eret does) so the kernel stack does not leak 256 bytes on
     * every yield. sp now points just past the frame; co_switch saves this
     * as the child's kernel context. */
    __asm__ volatile("add sp, %0, #0x100" :: "r"(g_el0_cur_frame) : "memory");
    /* Yield the child back to the parent (GUI / calling context) at the kernel
     * level. The parent's syscall 414 then returns to the GUI; the next co_resume
     * re-enters the child via hudos_el0_ctx_eret with the persisted full ctx. */
    co_switch(&g_co_child, &g_co_parent);
    if (g_el0_co) {                       /* resumed: back into the kernel (EL1) */
        __asm__ volatile("msr daifset, #0x3" ::: "memory");
        /* We are back at EL1 running kernel/syscall code, NOT yet executing the
         * child at EL0. Keep the FIRMWARE vectors live here; the eret back into
         * the child's EL0 window (handler pre-eret, or SYS_co_resume's ctx_eret
         * path at line ~4922) is what reinstalls g_el0_vbar. Installing g_el0_vbar
         * here would re-open the window where a firmware call inside the resumed
         * syscall runs with the wrong vector table and crashes. */
        el0_set_vbar(g_fw_vbar);
        /* Switch back to the child's isolated TTBR0 before re-entering EL0. The
         * parent ran on the firmware table (see above), so we must restore the
         * child clone here; otherwise the eret into EL0 would walk the firmware
         * table and lose the EL0 memory isolation. */
        if (g_el0_use_pt) el0_set_ttbr0(g_el0_child_ttbr0);
    }
}

/* Prepare a coroutine child to run at EL0. 0 on success; on failure the caller
 * keeps the existing EL1 coroutine behaviour. */
/* Phase B forward declarations (definitions live further down, after the walker). */
static unsigned long long el0_rd_ttbr0(void);
static int                 el0_build_child_pt(unsigned long long base,
                                             unsigned long long pages,
                                             unsigned long long pool);
static void                el0_set_ttbr0(unsigned long long v);
static void                el0_free_child_pt(void);
/* Diagnostic forward declarations (defined further down). */
static unsigned long long *el0_find_leaf_desc_root(unsigned long long root,
                                                   unsigned long long va,
                                                   unsigned long long *out_blksz);
static void                el0_dump_desc(const char *tag, unsigned long long *d,
                                         unsigned long long va);
static int                 el0_is_child(unsigned long long e_va, unsigned long long esz);

static int el0_co_setup(UINT64 base, UINT64 pages, VOID *pool)
{
    if (!g_el0_enable || !g_el0_vbar) {
        el0_puts("EL0_SETUP skip: enable="); el0_puthex("", (unsigned long long)g_el0_enable);
        el0_puthex(" vbar=", g_el0_vbar); el0_puts("\n");
        return -1;
    }
    el0_reset_ranges();
    /* Reset ALL per-child coroutine state so nothing leaks in from a PREVIOUS
     * child that already exited. The EL0 window / context structs are shared
     * singleton state, not per-instance: a prior child's g_el0_resume_mode,
     * g_el0_yield_pending, or its captured g_co_child_ctx must not survive into
     * the new child, or the new child's first resume can restore a foreign EL0
     * context and eret into garbage -> EL1 exception / freeze. This also guards
     * the filemgr->imgview style transition where the old child exits via SVC
     * #93 (which leaves these flags set) and a fresh child is launched next. */
    g_el0_resume_mode = 0;
    g_el0_yield_pending = 0;
    for (int i = 0; i < 32; i++) g_co_child_ctx.x[i] = 0;
    g_co_child_ctx.elr = 0;
    g_co_child_ctx.spsr = 0;
    g_co_child_ctx.sp_el0 = 0;
    g_el0_fw_ttbr0 = el0_rd_ttbr0();          /* capture the live firmware table */
    /* One kernel stack for EL0 children, allocated once and reused. */
    if (!g_el0_co_ksp) {
        VOID *buf = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiRuntimeServicesData, 32768 + 64, &buf)) || !buf)
            return -1;
        g_el0_co_ksp = (((UINT64)(UINTN)buf + 15ULL) & ~15ULL) + 32768;
    }
    /* ---- Phase B: private page table + ASID (preferred) ----
     * Clone the firmware TTBR0 tree and promote only the child image + argv pool to
     * EL0. This isolates the child completely regardless of where AllocateAnyPages
     * placed it, so the overlap that broke the additive scheme can no longer affect
     * firmware. On any failure we fall back to the additive grant below. */
    if (g_el0_phaseb && el0_build_child_pt(base, (unsigned long long)pages,
                           (unsigned long long)(unsigned long)pool) == 0) {
        g_el0_use_pt = 1;
        el0_set_ttbr0(g_el0_fw_ttbr0);        /* stay on firmware MMU until EL0 entry */
        el0_puts("EL0_SETUP PHASEB ok (isolated)\n");
        return 0;
    }
    /* Phase B build failed: free any partial clone and fall back. */
    el0_free_child_pt();
    g_el0_use_pt = 0;
    g_el0_child_ttbr0 = g_el0_fw_ttbr0;       /* asm switch becomes a no-op */
    el0_puts("EL0_SETUP PHASEB failed -> additive fallback\n");
    /* Safety audit (additive only): refuse a layout where the child allocation
     * overlaps firmware/parent/shadow/stack, since additive widens the firmware
     * table in place and would let the child overwrite that memory. */
    {
        unsigned long long cend = base + (unsigned long long)pages * 4096ULL;
        int bad = 0;
        el0_puts("CO_RANGE base="); el0_puthex("", base);
        el0_puthex(" end=", cend); el0_puts("\n");
        if (g_fw_vbar >= base && g_fw_vbar < cend) {
            el0_puts("*** OVERLAP: firmware VBAR page inside child range ***\n"); bad = 1;
        }
        if (g_parent_base && g_parent_base < cend && base < g_parent_end) {
            el0_puts("*** OVERLAP: parent image inside child range ***\n"); bad = 1;
        }
        if (g_shadow_base && g_shadow_base < cend && base < g_shadow_end) {
            el0_puts("*** OVERLAP: FB shadow inside child range ***\n"); bad = 1;
        }
        if (g_el0_co_ksp && (g_el0_co_ksp - 32768) < cend && base < g_el0_co_ksp) {
            el0_puts("*** OVERLAP: EL0 kernel stack inside child range ***\n"); bad = 1;
        }
        if (bad) {
            el0_ungrant_pages();
            return -1;
        }
    }
    if (el0_grant_pages(base, (unsigned long long)pages * 4096ULL) != 0) {
        el0_puts("EL0_SETUP image-grant FAILED base=");
        el0_puthex("", base); el0_puts(" pages="); el0_puthex("", (unsigned long long)pages);
        el0_puts("\n");
        el0_ungrant_pages(); return -1;
    }
    if (pool && el0_grant_pages((unsigned long long)(unsigned long)pool & ~0xFFFULL,
                                0x1000) != 0) { el0_ungrant_pages(); return -1; }
    return 0;
}

static void el0_co_teardown(void)
{
    if (g_el0_co) { el0_ungrant_pages(); g_el0_co = 0; g_el0_co_first = 0; }
    if (g_el0_use_pt) {
        el0_free_child_pt();
        g_el0_use_pt = 0;
        g_el0_child_ttbr0 = g_el0_fw_ttbr0;   /* asm switch no longer redirects */
    }
    el0_reset_ranges();
}
void hudos_el0_sync_handler(struct el0_frame *f) {
    unsigned long long esr = el0_mrs_esr();
    unsigned int ec = (unsigned int)((esr >> 26) & 0x3F);
    unsigned long long n;

    g_el0_cur_frame = f;   /* so el0_co_yield can persist the full EL0 state */
    /* Snapshot the child's SVC trap context BEFORE any firmware call can clobber
     * ELR_EL1/SPSR_EL1 (see g_el0_saved_elr/spsr comment). */
    g_el0_saved_elr  = el0_mrs_elr();
    g_el0_saved_spsr = el0_mrs_spsr();
    /* We have now trapped from EL0 into EL1. Per the VBAR window rule the child's
     * own vectors (g_el0_vbar) must ONLY be installed while the child actually runs
     * at EL0; at all other times (EL1 kernel + firmware calls) the FIRMWARE vectors
     * (g_fw_vbar) must be live. If we leave g_el0_vbar installed here, any exception
     * firmware raises inside its own Open()/boot-service call (it re-enables IRQs and
     * does eret, and on QEMU its disk I/O path takes a firmware-internal trap) is
     * routed into OUR 0x200 slot -> hudos_el1_unexp -> "EL1 EXCEPTION". This is the
     * real cause of the imgview open() crash (ELR lands in firmware msr daifclr/eret,
     * VBAR still g_el0_vbar). Switch to the firmware vectors for the whole EL1
     * servicing window; we restore g_el0_vbar right before the eret back to EL0. */
    el0_set_vbar(g_fw_vbar);
    if (!g_el0_quiet) { unsigned long long _v; __asm__ volatile("mrs %0, vbar_el1" : "=r"(_v));
      el0_puts("[sync] n="); el0_puthex("", (unsigned long long)f->x[8]);
      el0_puthex(" ec=", (unsigned long long)ec); el0_puthex(" elr=", (unsigned long long)el0_mrs_elr());
      el0_puthex(" spsr=", (unsigned long long)el0_mrs_spsr()); el0_puthex(" VBAR=", _v); el0_puts("\n"); }

    if (!g_el0_quiet) { el0_puts("el0: SYNC entry\n"); el0_puthex("el0: esr=", esr); }

    if (ec != 0x15) {                       /* anything else is a user fault */
        el0_puts("*** EL0 FAULT (ec=");
        el0_putc((char)('0' + (int)ec)); el0_putc(')'); el0_putc('\n');
        el0_puthex("ESR=", esr);
        el0_puthex("FAR=", el0_mrs_far());
        el0_puthex("ELR=", el0_mrs_elr());
        {
            unsigned long long far = el0_mrs_far();
            unsigned long long va, child_root, fw_root;
            child_root = g_el0_child_ttbr0 & ((1ULL<<48)-1) & ~0xFFFULL;
            fw_root    = g_el0_fw_ttbr0    & ((1ULL<<48)-1) & ~0xFFFULL;
            el0_puthex(" child_root=", child_root); el0_puthex(" fw_root=", fw_root);
            el0_puts("--- PTE dump (CHILD clone vs FW orig) ---\n");
            for (va = (far & ~0xFFFULL) - 0x3000; va <= (far & ~0xFFFULL) + 0x1000; va += 0x1000) {
                unsigned long long cbs, fbs;
                unsigned long long *cd = el0_find_leaf_desc_root(child_root, va, &cbs);
                unsigned long long *fd = el0_find_leaf_desc_root(fw_root,    va, &fbs);
                el0_dump_desc("  CHILD", cd, va);
                el0_dump_desc("  FW   ", fd, va);
            }
            el0_puts("--- end PTE dump ---\n");
        }
        hudos_el0_stats[1]++;
        hudos_el0_exit(-1);
        for (;;) __asm__ volatile("wfi");
    }

    n = f->x[8];
    hudos_el0_stats[0]++;

    /* Program termination. 93=exit/94=exit_group behave differently here than
     * at EL1: at EL1 they merely set g_elf_exited and RETURN (letting the
     * program unwind), but from EL0 we use it as the way back to the loader,
     * which matches POSIX semantics (exit must not return). 999 is the boot
     * self-test probe; it is never issued by a real program. */
    if (n == 93 || n == 94 || n == 999) {
        hudos_el0_stats[3]++;
        if (g_el0_co) {                      /* EL0 coroutine child finished */
            /* IMPORTANT: do NOT clear g_el0_active or call co_child_cleanup()
             * here. el0_co_yield() must still observe g_el0_active==1 and
             * g_el0_co==1 so its guard passes and it can co_switch back to the
             * parent; the child's live context is captured from the current trap
             * frame (f). Resource teardown is deferred to the parent's
             * SYS_co_resume, which frees the child when it sees g_co_state==3
             * (case 414). Clearing g_el0_co here made the guard reject the yield
             * and the child fell into "for(;;)wfi" — the "closing a program
             * freezes" bug. */
            el0_set_ttbr0(g_el0_fw_ttbr0);
            g_co_state = 3;
            el0_set_vbar(g_fw_vbar);
            __asm__ volatile("msr daifclr, #0x3" ::: "memory");
            /* The exit special-case runs BEFORE the normal path sets
             * g_el0_active=1 (line 1924). el0_co_yield()'s guard requires
             * g_el0_active==1 to co_switch back to the parent, so set it here.
             * Without it the guard rejects the yield and we fall into wfi —
             * the "closing a program freezes" bug (the child's SVC #93 exit
             * never hands control back to the GUI loop). */
            g_el0_active = 1;
            el0_co_yield(1);   /* does not return; yields back to the GUI loop */
            for (;;) __asm__ volatile("wfi");
        }
        g_el0_active = 0;
        hudos_el0_exit(n == 999 ? 0xC0DE : (long)f->x[0]);
        for (;;) __asm__ volatile("wfi");
    }

    g_el0_active = 1;
    f->x[0] = (unsigned long long)sys_dispatch((long)n, (long)f->x[0], (long)f->x[1],
                                               (long)f->x[2], (long)f->x[3], (long)f->x[4]);
    g_el0_active = 0;
    /* We are about to eret back into the EL0 child; it needs OUR vectors so its
     * next SVC #0 routes to hudos_el0_sync (0x400), not the firmware vectors.
     * First RESTORE the child's SVC trap context: the firmware call above may have
     * clobbered ELR_EL1/SPSR_EL1, so reload the snapshot taken at handler entry. */
    {
        __asm__ volatile("msr elr_el1,%0" :: "r"(g_el0_saved_elr) : "memory");
        __asm__ volatile("msr spsr_el1,%0" :: "r"(g_el0_saved_spsr) : "memory");
    }
    el0_set_vbar(g_el0_vbar);
    hudos_el0_eret(f);                       /* does not return */
    for (;;) __asm__ volatile("wfi");
}

/* Should never fire: IRQs are masked inside the EL0 window. */
void hudos_el0_async_handler(struct el0_frame *f) {
    el0_puts("*** EL0 ASYNC EXCEPTION slot=");
    el0_putc((char)('0' + (int)(f->x[31] & 0xF)));
    el0_putc('\n');
    /* ESR is UNKNOWN for asynchronous exceptions, so print it but do not trust
     * it: it may simply be a stale value rather than the cause. */
    el0_puthex("ESR(stale?)=", el0_mrs_esr());
    el0_puthex("ELR=", el0_mrs_elr());
    el0_puthex("DAIF=", el0_mrs_daif());
    el0_puthex("SP_EL0=", el0_mrs_sp_el0());
    hudos_el0_stats[2]++;
    hudos_el0_exit(-1);
    for (;;) __asm__ volatile("wfi");
}

/* An EL1 exception while our vectors are installed. Since IRQs are masked this
 * indicates a real kernel bug, but rather than spinning we try to unwind the
 * EL0 session cleanly so the rest of the OS survives. */
void hudos_el1_unexp_handler(struct el0_frame *f) {
    (void)f;
    unsigned long long esr = el0_mrs_esr();
    unsigned int ec = (unsigned int)((esr >> 26) & 0x3F);
    unsigned long long elr = el0_mrs_elr();
    unsigned long long cel = 0, spsr = 0, vbar = 0;
    __asm__ volatile("mrs %0, currentel" : "=r"(cel));
    __asm__ volatile("mrs %0, spsr_el1"  : "=r"(spsr));
    __asm__ volatile("mrs %0, vbar_el1"  : "=r"(vbar));
    el0_puts("*** EL1 EXCEPTION UNDER HUDOS VECTORS ***\n");
    el0_puthex("ESR=", esr);
    el0_puthex("FAR=", el0_mrs_far());
    el0_puthex("ELR=", elr);
    el0_puthex("CURRENT_EL=", cel);
    el0_puthex("SPSR=", spsr);
    el0_puthex("VBAR=", vbar);
    el0_puthex("child_entry=", (unsigned long long)(unsigned long)g_co_child_entry);
    el0_puthex("ELR-child=", (unsigned long long)(elr - (unsigned long long)(unsigned long)g_co_child_entry));
    el0_puthex("ELR&fff=", elr & 0xFFFULL);
    el0_puthex("g_fw_vbar=", g_fw_vbar);
    el0_puthex("LR(x30)=", *(const unsigned long long *)((const char *)f + 0xF0));
    el0_puthex("FP(x29)=", *(const unsigned long long *)((const char *)f + 0xE0));
    el0_puthex("g_el0_live=", (unsigned long long)g_el0_live);
    el0_puthex("g_el0_co=", (unsigned long long)g_el0_co);
    el0_puthex("g_co_state=", (unsigned long long)g_co_state);
    el0_puthex("g_el0_active=", (unsigned long long)g_el0_active);
    {
        const unsigned int *pc = (const unsigned int *)(unsigned long)elr;
        el0_puthex("INSN[-2]=", (unsigned long long)pc[-2]);
        el0_puthex("INSN[-1]=", (unsigned long long)pc[-1]);
        el0_puthex("INSN[0] =", (unsigned long long)pc[0]);
        el0_puthex("INSN[+1]=", (unsigned long long)pc[1]);
        el0_puthex("INSN[+2]=", (unsigned long long)pc[2]);
    }
    if (g_parent_base && elr >= g_parent_base && elr < g_parent_end)
        el0_puthex("ELR-in-parent @off=", elr - g_parent_base);
    if (g_child_base && elr >= g_child_base && elr < g_child_end)
        el0_puthex("ELR-in-child  @off=", elr - g_child_base);
    if (g_shadow_base && elr >= g_shadow_base && elr < g_shadow_end)
        el0_puthex("ELR-in-SHADOW @off=", elr - g_shadow_base);
    if (g_imgf_base && elr >= g_imgf_base && elr < g_imgf_end)
        el0_puthex("ELR-in-IMGFILE @off=", elr - g_imgf_base);
    if (g_argp_base && elr >= g_argp_base && elr < g_argp_end)
        el0_puthex("ELR-in-ARGPOOL @off=", elr - g_argp_base);
    if (g_el0_co_ksp && elr >= g_el0_co_ksp - 32768 && elr < g_el0_co_ksp)
        el0_puthex("ELR-in-KSTACK  @off=", elr - (g_el0_co_ksp - 32768));
    /* Dump the actual PTE at the faulting address (child vs firmware root) so we
     * can see the real AP/UXN bits. In additive mode both roots are the firmware
     * table, so this reveals whether the granted page is actually EL0-executable. */
    {
        unsigned long long far = el0_mrs_far();
        unsigned long long va, child_root, fw_root;
        child_root = g_el0_child_ttbr0 & ((1ULL<<48)-1) & ~0xFFFULL;
        fw_root    = g_el0_fw_ttbr0    & ((1ULL<<48)-1) & ~0xFFFULL;
        el0_puthex(" child_root=", child_root); el0_puthex(" fw_root=", fw_root);
        el0_puts("--- PTE dump @ faulting va ---\n");
        for (va = (far & ~0xFFFULL) - 0x3000; va <= (far & ~0xFFFULL) + 0x1000; va += 0x1000) {
            unsigned long long cbs, fbs;
            unsigned long long *cd = el0_find_leaf_desc_root(child_root, va, &cbs);
            unsigned long long *fd = el0_find_leaf_desc_root(fw_root,    va, &fbs);
            el0_dump_desc("  CHILD", cd, va);
            el0_dump_desc("  FW   ", fd, va);
        }
        el0_puts("--- end PTE dump ---\n");
    }
    el0_puthex("parent_base=", g_parent_base);
    el0_puthex("child_base =", g_child_base);
    el0_puthex("shadow     =", g_shadow_base);
    el0_puthex("shadow_end =", g_shadow_end);
    el0_puthex("imgfile    =", g_imgf_base);
    el0_puthex("argpool    =", g_argp_base);
    el0_puthex("kstack_top =", g_el0_co_ksp);
    {   /* hexdump around the crash PC */
        const unsigned int *pc = (const unsigned int *)(unsigned long)elr;
        int k;
        el0_puts("ELR dump:\n");
        for (k = -8; k <= 8; k++) {
            el0_puthex("  ", (unsigned long long)(k * 4));
            el0_puthex(" ", (unsigned long long)pc[k]);
            el0_puts("\n");
        }
    }
    if (g_fw_vbar) el0_set_vbar(g_fw_vbar);
    if (g_el0_live) {
        g_el0_live = 0;
        hudos_el0_exit(-1);
    }
    for (;;) __asm__ volatile("wfi");
}

/* Boot self-test: drop to EL0, trap straight back, verify we get control. */
static unsigned long long g_el0_test_stack[256];
/* ===================================================================
 * Minimal, additive paging change for EL0.
 *
 * hudos must not replace the firmware's translation tables (that attempt died
 * earlier), but it cannot run user code at EL0 while UEFI has every page mapped
 * AP=0b00 (EL1 read/write, EL0 no access) — a program touching its own data or
 * stack aborts with DFSC=0xF (permission fault, as measured).
 *
 * So instead of taking over the MMU we WALK the firmware's existing stage-1
 * tables down to the leaf descriptor for an address and widen it in place:
 * AP[2:1] 0b00 -> 0b01 (EL1 RW + EL0 RW) and UXN cleared so the program can
 * execute its own code. This is strictly ADDITIVE — it never removes any access
 * the firmware depends on — which is why it is safe with boot services live.
 * Every original descriptor is recorded so el0_ungrant_pages() restores it.
 * Supports 4KB granules / TTBR0; anything else refuses and we stay at EL1.
 * =================================================================== */
#define EL0_GMAX 16384
static unsigned long long *g_el0_desc[EL0_GMAX];
static unsigned long long  g_el0_orig[EL0_GMAX];
static unsigned long long  g_el0_split_pa[EL0_GMAX];   /* PA of table allocated by a block split; 0 = none */
static int                 g_el0_gn = 0;

static unsigned long long el0_rd_tcr(void)   { unsigned long long v; __asm__ volatile("mrs %0, tcr_el1"   : "=r"(v)); return v; }
static unsigned long long el0_rd_ttbr0(void) { unsigned long long v; __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v)); return v; }
static unsigned long long el0_rd_ttbr1(void) { unsigned long long v; __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(v)); return v; }

static unsigned long long *el0_find_leaf_desc(unsigned long long va, unsigned long long *out_blksz)
{
    unsigned long long tcr = el0_rd_tcr();
    int tg0 = (int)((tcr >> 14) & 3);
    int t0sz = (int)(tcr & 0x3F);
    int vbits = 64 - t0sz;
    unsigned long long base, mask, desc;
    int level, start;

    if (out_blksz) *out_blksz = 0x1000;
    if (tg0 != 0) return 0;                     /* 4KB granule only */
    if (vbits < 25 || vbits > 48) return 0;

    mask = (1ULL << (64 - vbits)) - 1ULL;
    base = (((va >> vbits) == mask) ? el0_rd_ttbr1() : el0_rd_ttbr0());
    base &= ((1ULL << 48) - 1ULL) & ~0xFFFULL;  /* BADDR (4KB granule) */

    start = 4 - ((vbits - 12 + 8) / 9);
    if (start < 0) start = 0;

    for (level = start; level <= 3; level++) {
        int shift = 12 + 9 * (3 - level);
        unsigned long long *slot = &((unsigned long long *)base)[(va >> shift) & 0x1FF];
        desc = *slot;
        if ((desc & 3ULL) == 0ULL) break;                   /* invalid entry */
        if (level < 3 && (desc & 3ULL) == 1ULL) {          /* block leaf   */
            if (out_blksz) *out_blksz = (1ULL << shift);
            return slot;
        }
        if ((desc & 3ULL) == 2ULL) break;
        if (level == 3) return slot;                         /* page leaf    */
        base = desc & ((1ULL << 48) - 1ULL) & ~0xFFFULL;     /* descend      */
    }
    return 0;
}

/* Root-parameter variant: walk a table tree rooted at a given PA instead of the
 * live TTBR0. Used by the fault diagnostic to compare the child clone against the
 * original firmware tree without depending on the current MMU state. */
static unsigned long long *el0_find_leaf_desc_root(unsigned long long root,
                                                   unsigned long long va,
                                                   unsigned long long *out_blksz)
{
    unsigned long long tcr = el0_rd_tcr();
    int tg0 = (int)((tcr >> 14) & 3);
    int t0sz = (int)(tcr & 0x3F);
    int vbits = 64 - t0sz;
    unsigned long long base, desc;
    int level, start;

    if (out_blksz) *out_blksz = 0x1000;
    if (tg0 != 0) return 0;
    if (vbits < 25 || vbits > 48) return 0;
    base = root & ((1ULL << 48) - 1ULL) & ~0xFFFULL;
    start = 4 - ((vbits - 12 + 8) / 9);
    if (start < 0) start = 0;
    for (level = start; level <= 3; level++) {
        int shift = 12 + 9 * (3 - level);
        unsigned long long *slot = &((unsigned long long *)base)[(va >> shift) & 0x1FF];
        desc = *slot;
        if ((desc & 3ULL) == 0ULL) break;
        if (level < 3 && (desc & 3ULL) == 1ULL) {
            if (out_blksz) *out_blksz = (1ULL << shift);
            return slot;
        }
        if ((desc & 3ULL) == 2ULL) break;
        if (level == 3) return slot;
        base = desc & ((1ULL << 48) - 1ULL) & ~0xFFFULL;
    }
    return 0;
}

/* Decode + print one descriptor for the fault diagnostic. */
static void el0_dump_desc(const char *tag, unsigned long long *d, unsigned long long va)
{
    if (!d) { el0_puts(tag); el0_puts(" va="); el0_puthex("", va); el0_puts(" NONE\n"); return; }
    unsigned long long x = *d;
    int type = (int)(x & 3ULL);
    int ap   = (int)((x >> 6) & 3ULL);
    int uxn  = (int)((x >> 54) & 1ULL);
    int pxn  = (int)((x >> 53) & 1ULL);
    el0_puts(tag); el0_puthex(" va=", va); el0_puthex(" desc=", x);
    el0_puts(" type="); el0_putc((char)('0' + (type >> 1))); el0_putc((char)('0' + (type & 1)));
    el0_puts(" ap="); el0_putc((char)('0' + ap));
    el0_puts(" uxn="); el0_putc((char)('0' + uxn));
    el0_puts(" pxn="); el0_putc((char)('0' + pxn));
    el0_puts(" child="); el0_putc((char)('0' + (int)el0_is_child(va, 0x1000)));
    el0_puts("\n");
}

static void el0_tlb_flush(void)
{
    __asm__ volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

/* Demote a firmware BLOCK descriptor at *parent into a 4KB page table so we can
 * grant EL0 access to just the child's pages inside it. Widening the whole block
 * in place (the old behaviour) handed the child write access to unrelated
 * kernel/gui code sharing the same 2MB SECTION and let a buffer overrun corrupt
 * the kernel — the intermittent EL1 crash we chased. Replacing the block with a
 * table of 4KB pages preserves the firmware's exact mapping (EL1 still sees
 * identical PA/attributes) while letting us narrow EL0 down to specific pages.
 *
 * Only 2MB (level-2) SECTION blocks are split here; if the firmware ever uses a
 * larger (1GB) block we refuse and the caller falls back to EL1 for that child,
 * which is safe. */
static int el0_split_block(unsigned long long *parent, unsigned long long blksz,
                           EFI_PHYSICAL_ADDRESS *out_tbl)
{
    if (blksz != 0x200000ULL) {
        el0_puts("SPLIT_REFUSE blksz="); el0_puthex("", blksz); el0_puts("\n");
        return -1;                  /* 2MB SECTION only */
    }
    unsigned long long block = *parent;
    unsigned long long base_pa = block & ((1ULL << 48) - 1) & ~0x1FFFFFULL; /* 2MB-aligned PA */
    /* Attributes common to block and 4KB page descriptors: [11:2] nG/AF/SH/AP/NS
     * plus XN(54)/PXN(53)/Contiguous(52)/DBM(51). PBHA[62:59] left as 0. */
    unsigned long long attr = (block & 0xFFFULL) & ~3ULL;
    attr |= block & ((1ULL << 51) | (1ULL << 52) | (1ULL << 53) | (1ULL << 54));
    EFI_PHYSICAL_ADDRESS pa = 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &pa)) || !pa) {
        el0_puts("SPLIT_ALLOC_FAIL\n");
        return -1;
    }
    unsigned long long *tbl = (unsigned long long *)(unsigned long)pa;
    int i;
    for (i = 0; i < 512; i++)
        tbl[i] = (base_pa + (unsigned long long)i * 0x1000ULL) | attr | 3ULL;  /* page = 0b11 */
    *out_tbl = pa;
    *parent = (pa & ~0xFFFULL) | 3ULL;                   /* install table descriptor */
    return 0;
}

static int el0_grant_pages(unsigned long long va, unsigned long long len)
{
    unsigned long long a, end = va + len;
    unsigned long long last_blk = 0;
    int gated = 0;

    /* Walk the firmware's stage-1 tables 4KB at a time. For any page that lives
     * inside a firmware BLOCK (e.g. a 2MB SECTION), demote that block to a 4KB
     * page table ONCE and then widen only the child's exact pages — never the
     * whole block. The block's original descriptor is recorded (and its split
     * table freed) so el0_ungrant_pages() restores the firmware mapping exactly.
     * Already-EL0-RW pages (e.g. a shadow re-granted by a later gop_info) are
     * skipped so the descriptor budget is not exhausted by repeated grants. */
    for (a = va & ~0xFFFULL; a < end; a += 0x1000ULL) {
        unsigned long long blksz = 0x1000ULL;
        unsigned long long *d = el0_find_leaf_desc(a, &blksz);
        if (!d) { gated = 1; break; }
        int split_this = 0;                               /* page sits in a block WE split */
        if (blksz > 0x1000ULL) {                         /* inside a firmware block */
            if ((unsigned long long)d != last_blk) {
                unsigned long long orig = *d;            /* capture before split mutates it */
                EFI_PHYSICAL_ADDRESS tbl = 0;
                if (g_el0_gn >= EL0_GMAX) { gated = 1; break; }
                if (el0_split_block(d, blksz, &tbl) != 0) { gated = 1; break; }
                g_el0_desc[g_el0_gn] = d;               /* record the L2 entry ONLY */
                g_el0_orig[g_el0_gn] = orig;
                g_el0_split_pa[g_el0_gn] = tbl;
                g_el0_gn++;
                last_blk = (unsigned long long)d;
                split_this = 1;
                d = el0_find_leaf_desc(a, &blksz);       /* resolve into the new table */
                if (!d) { gated = 1; break; }
            } else {
                split_this = 1;
                d = el0_find_leaf_desc(a, &blksz);       /* already split: resolve page */
                if (!d) { gated = 1; break; }
            }
        }
        /* Idempotent: already EL0-RW? leave it. Pages inside a split block must NOT
         * be recorded (restoring the L2 entry undoes the whole split), so we skip
         * both the record and the re-widen here. */
        if (((*d) & (3ULL << 6)) == (1ULL << 6) && !((*d) & (1ULL << 54)))
            continue;
        if (!split_this && blksz <= 0x1000ULL) {         /* a genuine 4KB firmware page */
            if (g_el0_gn >= EL0_GMAX) { gated = 1; break; }
            g_el0_desc[g_el0_gn] = d;
            g_el0_orig[g_el0_gn] = *d;
            g_el0_split_pa[g_el0_gn] = 0;
            g_el0_gn++;
        }
        /* AP[2:1] 0b00 -> 0b01 (EL1+EL0 RW); clear UXN so the child can exec. */
        *d = ((*d & ~(3ULL << 6)) | (1ULL << 6)) & ~(1ULL << 54);
    }
    if (g_el0_rn < EL0_RMAX) {
        unsigned long long st = va & ~0xFFFULL;
        g_el0_rstart[g_el0_rn] = st;
        g_el0_rlen[g_el0_rn]   = (end - st + 0xFFFULL) & ~0xFFFULL;
        g_el0_rn++;
    }
    el0_tlb_flush();
    return gated ? -1 : 0;
}

static void el0_ungrant_pages(void)
{
    int i;
    for (i = 0; i < g_el0_gn; i++) {
        if (g_el0_desc[i]) *g_el0_desc[i] = g_el0_orig[i];
        if (g_el0_split_pa[i])
            gBS->FreePages((EFI_PHYSICAL_ADDRESS)g_el0_split_pa[i], 1);
    }
    g_el0_gn = 0;
    el0_tlb_flush();
}

/* ===================================================================
 * Phase B: independent page table + ASID for the EL0 coroutine child.
 *
 * The additive scheme (el0_grant_pages) widened the firmware's OWN stage-1 table
 * in place, which necessarily kept firmware/kernel memory EL0-accessible. For a
 * large ELF (imgview's ~42MB BSS) AllocateAnyPages can hand back a range that
 * overlaps firmware code; the child then corrupts firmware on any stray write,
 * and a corrupted return address executes firmware code (a NULL-deref crash at
 * best). Phase B removes that entire class of bug:
 *
 *   clone the firmware TTBR0 tree into a private tree, promote ONLY the child's
 *   own [base,end) image range (and the argv pool) to EL0-RW+X, and demote every
 *   other descriptor to EL1-only + UXN. Install it with a child ASID. Now the
 *   child's EL0 address space contains exactly itself; any out-of-range access or
 *   branch becomes a clean EL0 fault instead of silently running firmware.
 *
 * The cloned tree keeps everything EL1-accessible (identical firmware mapping,
 * just with EL0 stripped), so the SVC handler and the parent GUI run unchanged.
 * Build failure => fall back to the additive grant (and ultimately EL1).
 * =================================================================== */
#define MASK48 (((unsigned long long)1 << 48) - 1)

static int el0_is_child(unsigned long long e_va, unsigned long long esz)
{
    int a = (e_va < g_pt_cend) && (e_va + esz > g_pt_cbase);
    int b = g_pt_pbase && (e_va < g_pt_pend) && (e_va + esz > g_pt_pbase);
    return a || b;
}

/* AP[2:1] -> EL1 RW only (0b00); set UXN so EL0 cannot execute or read it. */
static unsigned long long el0_demote(unsigned long long d)
{
    return (d & ~(3ULL << 6)) | (1ULL << 54);
}
/* AP[2:1] -> EL1+EL0 RW (0b01); clear UXN so the child can execute its own code. */
static unsigned long long el0_child(unsigned long long d)
{
    return (d & ~(3ULL << 6) & ~(1ULL << 54)) | (1ULL << 6);
}

static unsigned long long el0_alloc_table(void)
{
    EFI_PHYSICAL_ADDRESS np = 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &np)) || !np)
        return 0;
    if (g_el0_pt_n < EL0_PT_MAX) g_el0_pt_pages[g_el0_pt_n++] = np;
    return (unsigned long long)np;
}

/* Split a firmware BLOCK (level<3) into a fresh level+1 table that replicates its
 * contiguous PA mapping, then demote/promote each sub-entry against the child
 * regions. Returns the new table PA or 0 on failure. */
static unsigned long long el0_split(unsigned long long desc, int level,
                                    unsigned long long e_va);

/* In-place demote/promote of a freshly-built (flat) table with NO sub-tables. */
static int el0_demote_inplace(unsigned long long *tbl, int level,
                              unsigned long long va,
                              unsigned long long cbase, unsigned long long cend);

/* Deep-clone the firmware table at fw_pa into a fresh table, demoting/promoting. */
static unsigned long long el0_clone_table(unsigned long long fw_pa, int level,
                                          unsigned long long va,
                                          unsigned long long cbase, unsigned long long cend);

static unsigned long long el0_split(unsigned long long desc, int level,
                                    unsigned long long e_va)
{
    unsigned long long np = el0_alloc_table();
    if (!np) return 0;
    unsigned long long *t = (unsigned long long *)(unsigned long)np;
    unsigned long long blksz = (1ULL << (12 + 9 * (3 - level)));
    unsigned long long pa = desc & MASK48 & ~(blksz - 1);
    unsigned long long attr = (desc & 0xFFFULL) & ~3ULL;
    attr |= desc & ((1ULL << 51) | (1ULL << 52) | (1ULL << 53) | (1ULL << 54));
    unsigned long long sub_esz = 1ULL << (12 + 9 * (2 - level));
    unsigned long long sub_type = (level + 1 == 3) ? 3ULL : 1ULL; /* L3: page (0b11); else: block (0b01) */
    for (int i = 0; i < 512; i++)
        t[i] = (pa + (unsigned long long)i * sub_esz) | attr | sub_type;
    if (el0_demote_inplace(t, level + 1, e_va, g_pt_cbase, g_pt_cend)) return 0;
    return np;
}

static int el0_demote_inplace(unsigned long long *tbl, int level,
                              unsigned long long va,
                              unsigned long long cbase, unsigned long long cend)
{
    unsigned long long esz = 1ULL << (12 + 9 * (3 - level));
    for (int i = 0; i < 512; i++) {
        unsigned long long desc = tbl[i];
        int type = (int)(desc & 3ULL);
        if (type == 0) continue;
        unsigned long long e_va = va + (unsigned long long)i * esz;
        int child = el0_is_child(e_va, esz);
        if (level < 3 && type == 1) {                 /* block */
            if (!child) tbl[i] = el0_demote(desc);
            else if (e_va >= cbase && e_va + esz <= cend) tbl[i] = el0_child(desc);
            else {
                unsigned long long sub = el0_split(desc, level, e_va);
                if (!sub) return -1;
                tbl[i] = (sub & ~0xFFFULL) | 3ULL;
            }
        } else {                                       /* page leaf (or block@L3) */
            tbl[i] = child ? el0_child(desc) : el0_demote(desc);
        }
    }
    return 0;
}

static unsigned long long el0_clone_table(unsigned long long fw_pa, int level,
                                          unsigned long long va,
                                          unsigned long long cbase, unsigned long long cend)
{
    const unsigned long long *fw = (const unsigned long long *)(unsigned long)fw_pa;
    unsigned long long np = el0_alloc_table();
    if (!np) return 0;
    unsigned long long *nt = (unsigned long long *)(unsigned long)np;
    unsigned long long esz = 1ULL << (12 + 9 * (3 - level));
    for (int i = 0; i < 512; i++) {
        unsigned long long desc = fw[i];
        int type = (int)(desc & 3ULL);
        if (type == 0) { nt[i] = 0; continue; }
        unsigned long long e_va = va + (unsigned long long)i * esz;
        int child = el0_is_child(e_va, esz);
        if (level < 3 && (type == 2 || type == 3)) { /* firmware sub-table.
            * This EDK2 build encodes sub-tables as type 0b11 (not the spec's 0b10)
            * at every level; treat both as tables and deep-clone. The walker already
            * descends through 0b11, so we must too or the child's subtree is never
            * copied and stays EL1-only (the "clone == verbatim copy" bug). */
            unsigned long long sub = el0_clone_table(desc & MASK48 & ~0xFFFULL,
                                                    level + 1, e_va, cbase, cend);
            if (!sub) return 0;
            nt[i] = (sub & ~0xFFFULL) | 3ULL;
        } else if (level < 3 && type == 1) {          /* block */
            if (!child) nt[i] = el0_demote(desc);
            else if (e_va >= cbase && e_va + esz <= cend) nt[i] = el0_child(desc);
            else {
                unsigned long long sub = el0_split(desc, level, e_va);
                if (!sub) return 0;
                nt[i] = (sub & ~0xFFFULL) | 3ULL;
            }
        } else {                                       /* page leaf (or block@L3) */
            nt[i] = child ? el0_child(desc) : el0_demote(desc);
        }
    }
    return np;
}

/* Build the child-private page table. Returns 0 on success, -1 on any failure. */
static int el0_build_child_pt(unsigned long long base, unsigned long long pages,
                              unsigned long long pool)
{
    unsigned long long tcr = el0_rd_tcr();
    if (((tcr >> 14) & 3) != 0) return -1;            /* 4KB granule only */
    int t0sz = (int)(tcr & 0x3F);
    int vbits = 64 - t0sz;
    if (vbits < 25 || vbits > 48) return -1;
    int start = 4 - ((vbits - 12 + 8) / 9);
    if (start < 0) start = 0;

    g_pt_cbase = base;
    g_pt_cend  = base + pages * 4096ULL;
    g_pt_pbase = pool ? (pool & ~0xFFFULL) : 0;
    g_pt_pend  = g_pt_pbase ? (g_pt_pbase + 0x1000ULL) : 0;

    unsigned long long fw0 = el0_rd_ttbr0() & MASK48 & ~0xFFFULL;
    g_el0_pt_n = 0;
    unsigned long long root = el0_clone_table(fw0, start, 0, g_pt_cbase, g_pt_cend);
    if (!root) return -1;

    int as16 = (int)((tcr >> 36) & 1);                /* TCR.AS: 8-bit (56) or 16-bit (48) */
    int asid_shift = as16 ? 48 : 56;
    g_el0_child_pt = root;
    g_el0_child_ttbr0 = (root & ~0xFFFULL) | (1ULL << asid_shift);
    el0_puts("PHASEB pt="); el0_puthex("", root);
    el0_puthex(" asid_shift=", (unsigned long long)asid_shift);
    el0_puthex(" cbase=", base); el0_puthex(" cend=", g_pt_cend);
    if (g_pt_pbase) el0_puthex(" pool=", g_pt_pbase);
    el0_puts("\n");
    return 0;
}

static void el0_free_child_pt(void)
{
    for (int i = 0; i < g_el0_pt_n; i++)
        gBS->FreePages((EFI_PHYSICAL_ADDRESS)g_el0_pt_pages[i], 1);
    g_el0_pt_n = 0;
    g_el0_child_pt = 0;
}

/* Switch TTBR0_EL1 (full TLB flush). Used to install the child table on EL0 entry
 * and to restore the firmware table on exit. */
static void el0_set_ttbr0(unsigned long long v)
{
    __asm__ volatile("msr ttbr0_el1, %0\n\t"
                     "dsb ish\n\t"
                     "tlbi vmalle1is\n\t"
                     "dsb ish\n\t"
                     "isb\n"
                     :: "r"(v) : "memory");
}



static int el0_selftest(void) {
    long rc;
    if (!g_fw_vbar) { el0_puts("el0: no fw vbar\n"); return -1; }

    rc = el0_run((unsigned long long)(unsigned long)hudos_el0_selftest_entry,
                 (unsigned long long)(unsigned long)&g_el0_test_stack[256],
                 0, 0, 0);
    return (rc == 0xC0DE) ? 0 : (int)rc;
}

static void child_done(void);            /* defined below */
static void co_child_cleanup(void);      /* defined below */
void co_switch(coctx_t *from, coctx_t *to);



__asm__ (
 ".globl co_switch\n"
 "co_switch:\n"
 "  mov x2, sp\n"
 "  str x2, [x0, #96]\n"
 "  stp x19, x20, [x0, #0]\n"
 "  stp x21, x22, [x0, #16]\n"
 "  stp x23, x24, [x0, #32]\n"
 "  stp x25, x26, [x0, #48]\n"
 "  stp x27, x28, [x0, #64]\n"
 "  stp x29, x30, [x0, #80]\n"
 "  ldp x19, x20, [x1, #0]\n"
 "  ldp x21, x22, [x1, #16]\n"
 "  ldp x23, x24, [x1, #32]\n"
 "  ldp x25, x26, [x1, #48]\n"
 "  ldp x27, x28, [x1, #64]\n"
 "  ldp x29, x30, [x1, #80]\n"
 "  ldr x2, [x1, #96]\n"
 "  mov sp, x2\n"
 "  ret\n"
);

/* Entry shim for a coroutine child: set up _start's expected registers
 * (x0=argc=0, x1=argv=NULL, x2=envp=NULL, x3 = syscall dispatch pointer,
 *  x4 = child stack top, x30 = child_done) and branch to the ELF entry.
 * Runs on the child stack; never returns here (child main returns to _start
 * which returns to child_done).
 *
 * IMPORTANT: co_switch only preserves callee-saved x19..x30 + sp, NOT the
 * caller-saved x0..x18. So the child's startup registers (argc/argv/envp)
 * must be set HERE, exactly like call_elf() does, or main() will see garbage.
 * This matters for programs such as word.elf whose main() dereferences argv. */
static void __attribute__((used, noinline)) child_entry_trampoline(void) {
    register UINT64 argc asm("x0")  = g_co_child_argc;
    register UINT64 argv asm("x1")  = (UINT64)g_co_child_argv;
    register UINT64 envp asm("x2")  = 0;
    register UINT64 sd   asm("x3")  = (UINT64)sys_dispatch;
    register UINT64 stk  asm("x4")  = g_co_child_stack;
    register UINT64 done asm("x30") = (UINT64)child_done;
    __asm__ volatile ("br %6\n"
                      :: "r"(argc), "r"(argv), "r"(envp), "r"(sd), "r"(stk),
                         "r"(done), "r"(g_co_child_entry));
}

/* Called when the child's main() returns. We are running in EL0 (the trampoline
 * set x30 to this kernel function and the child image was granted EL0-X), NOT
 * inside an SVC trap handler, so g_el0_active==0 and el0_co_yield()'s guard would
 * reject it (which used to drop us into an infinite "wfi" = the close freeze).
 * Trap back into the kernel with SVC #93 (exit) so hudos_el0_sync_handler routes
 * us through the proper exit path, which yields to the parent and is torn down
 * there. */
static void child_done(void) {
    __asm__ volatile("mov x8, #93\n svc #0" ::: "x8", "memory");
    for (;;) __asm__ volatile("wfi");   /* does not return */
}

/* Free the child's image / stack and reset per-child state. */
static void co_child_cleanup(void) {
    el0_co_teardown();
    if (g_co_child_img) { gBS->FreePool(g_co_child_img); g_co_child_img = NULL; }
    if (g_co_child_base) { gBS->FreePages(g_co_child_base, g_co_child_pages); g_co_child_base = 0; g_co_child_pages = 0; }
    if (g_co_argv_pool) { gBS->FreePool(g_co_argv_pool); g_co_argv_pool = NULL; }
    g_co_child_argc = 0; g_co_child_argv = NULL;
    for (int i = 0; i < MAXFD; i++) if (g_fds[i]) { g_fds[i]->Close(g_fds[i]); g_fds[i] = NULL; }
    g_exec_out = NULL;
    g_co_yield_on_input = 0;
}

static int alloc_fd(void) {
    for (int i = 3; i < MAXFD; i++) if (g_fds[i] == NULL) return i;
    return -1;
}

/* ===================================================================
 * Graphics (GOP framebuffer) + mouse (absolute pointer)
 * =================================================================== */
static void gfx_try_init(void) {
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS st = gBS->LocateProtocol(&gopGuid, NULL, (void **)&gGop);
    if (EFI_ERROR(st)) { gGop = NULL; gFbBase = 0; return; }

    /* Pick a higher-resolution GOP mode so the desktop is twice as large as the
     * firmware default. Prefer exactly 2x the current WxH (i.e. 800x600 -> 1600x1200);
     * if no such mode exists, fall back to the largest mode that is at least 1024x768.
     * This is what makes "double the screen size" happen on the QEMU/cocoa window. */
    {
        /* Target a 1600x1200 desktop (a clear ~2x over the 800x600 baseline and
         * what the window layout is sized for). The ramfb device only enumerates
         * up to 1024x768, so run.sh now uses virtio-gpu-pci, which enumerates
         * 1600x1200 (and far higher). Fall back to the largest >=1024x768 mode. */
        UINTN tgtW = 1600, tgtH = 1200;
        UINT32 bestMode = 0, bestArea = 0;
        UINT32 maxMode  = 0, maxArea  = 0;
        for (UINT32 i = 0; i < gGop->Mode->MaxMode; i++) {
            UINTN sz = 0;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            if (EFI_ERROR(gGop->QueryMode(gGop, i, &sz, &info))) continue;
            UINTN w = info->HorizontalResolution, h = info->VerticalResolution;
            UINTN area = w * h;
            if (w >= 1024 && h >= 768 && area > maxArea) { maxMode = i; maxArea = area; }
            if (w >= tgtW && h >= tgtH) {
                if (bestMode == 0 || area < bestArea) { bestMode = i; bestArea = area; }
            }
        }
        UINT32 chosen = bestMode ? bestMode : maxMode;
        if (chosen) gGop->SetMode(gGop, chosen);
    }

    gFbBase   = gGop->Mode->FrameBufferBase;
    gFbW      = gGop->Mode->Info->HorizontalResolution;
    gFbH      = gGop->Mode->Info->VerticalResolution;
    gFbStride = gGop->Mode->Info->PixelsPerScanLine;
    gFbFormat = gGop->Mode->Info->PixelFormat;
    /* Some GOP devices (notably virtio-gpu-pci) report FrameBufferBase==0:
     * they are Blt-only and have no CPU-accessible linear framebuffer. In that
     * case allocate a kernel-side shadow buffer and render into it; the screen
     * is updated via GOP Blt on each input/sleep syscall (see gop_present). */
    gFbBltOnly = (gFbBase == 0) ? 1 : 0;
    if (gFbBltOnly) {
        gFbShadowSize = (UINTN)gFbStride * (UINTN)gFbH * 4;
        if (!EFI_ERROR(gBS->AllocatePool(EfiLoaderData, gFbShadowSize, (void **)&gFbShadow)) && gFbShadow) {
            gBS->SetMem(gFbShadow, gFbShadowSize, 0);
            g_shadow_base = (unsigned long long)(UINTN)gFbShadow;
            g_shadow_end  = g_shadow_base + (unsigned long long)gFbShadowSize;
        }
        else
            gFbShadow = NULL;
    }
    EFI_GUID absGuid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    st = gBS->LocateProtocol(&absGuid, NULL, (void **)&gAbs);
    if (!EFI_ERROR(st)) { gAbs->Reset(gAbs, FALSE); }
    else gAbs = NULL;
    con_print_ascii("gfx: GOP ");
    con_print_dec(gFbW); con_print_ascii("x"); con_print_dec(gFbH);
    con_print_ascii(" (stride "); con_print_dec(gFbStride);
    con_print_ascii(gFbBltOnly ? ", blt-only\r\n" : ", fb\r\n");
    con_print_ascii("gfx: mouse ");
    con_print_ascii(gAbs ? "yes\r\n" : "no\r\n");
    kcon_init();
}

static void fb_set_px(UINT32 x, UINT32 y, UINT32 rgb) {
    if (x >= gFbW || y >= gFbH) return;
    UINT8 r = (rgb>>16)&0xff, g = (rgb>>8)&0xff, b = rgb&0xff;
    UINT32 off = (y * gFbStride + x) * 4;
    if (gFbBltOnly) {                /* Blt-only device: draw into shadow buffer */
        if (!gFbShadow) return;
        /* GOP Blt expects source pixels in BLT_PIXEL order: Blue,Green,Red,Reserved */
        gFbShadow[off]=b; gFbShadow[off+1]=g; gFbShadow[off+2]=r; gFbShadow[off+3]=0xff;
        gFbDirty = 1;
        return;
    }
    if (!gFbBase) return;
    UINT8 *fb = (UINT8 *)(UINTN)gFbBase;
    if (gFbFormat == 0) {            /* PixelRedGreenBlueReserved8BitPerColor */
        fb[off]=r; fb[off+1]=g; fb[off+2]=b; fb[off+3]=0xff;
    } else {                         /* BGRx / bitmask -> treat as BGRx */
        fb[off]=b; fb[off+1]=g; fb[off+2]=r; fb[off+3]=0xff;
    }
}
static void gop_present(void) {
    /* Blt the shadow framebuffer to a Blt-only GOP device. Called once per
     * input/sleep syscall so the whole frame is flushed together (no tearing). */
    if (!gFbBltOnly || !gFbShadow || !gGop || !gFbDirty) return;
    /* Delta is in BYTES per source scanline, not pixels. Shadow is laid out as
     * gFbStride pixels (== gFbStride*4 bytes) per row, so pass gFbStride*4.
     * Passing gFbStride (pixels) would make the firmware stride the buffer by
     * only 1/4 the correct width -> garbled/tilted output. */
    gGop->Blt(gGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)gFbShadow,
              EfiBltBufferToVideo, 0, 0, 0, 0, gFbW, gFbH, gFbStride * 4);
    gFbDirty = 0;
}
static void fb_fill(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 rgb) {
    for (UINT32 yy = 0; yy < h; yy++)
        for (UINT32 xx = 0; xx < w; xx++)
            fb_set_px(x+xx, y+yy, rgb);
}

/* ===================================================================
 * Framebuffer text console (kcon)
 * A CJK-capable terminal rendered directly into the GOP framebuffer, using the
 * very same glyph style as tdesktop (16x32 cells; 8x16 ASCII scaled 2x; 32x32
 * CJK glyphs from cjk16x16.h). This makes the kernel shell show Chinese exactly
 * like the desktop does. Only active when g_kcon_enable is set (i.e. the shell
 * owns the screen) so it never clobbers the GUI in normal boot.
 * =================================================================== */
static unsigned kcon_vga(int idx) {
    static const unsigned pal[16] = {
        0x000000,0x0000AA,0x00AA00,0x00AAAA,
        0xAA0000,0xAA00AA,0xA5A500,0xAAAAAA,
        0x555555,0x5555FF,0x55FF55,0x55FFFF,
        0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF,
    };
    if (idx < 0) idx = 0; if (idx > 15) idx = 15;
    return pal[idx];
}
#define KATTR(f,b) ((unsigned char)(((b & 0xF) << 4) | (f & 0xF)))

static const unsigned int *kcon_cjk_glyph(unsigned cp) {
    int lo = 0, hi = CJK_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned c = CJK_CODEP[mid];
        if (c == cp) return &CJK_GLYPH[mid * 32];
        else if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static void kcon_draw_cell(int c, int r) {
    if (c < 0 || r < 0 || c >= g_kcols || r >= g_krows) return;
    int fg = g_kfg, bg = g_kbg;
    int at = g_kat[r][c]; fg = at & 0xF; bg = (at >> 4) & 0xF;
    int px = c * KCELLW, py = r * KCELLH;
    if (g_kwide[r][c] == 2) {                 /* CJK trail: owned by lead cell */
        fb_fill(px, py, KCELLW, KCELLH, kcon_vga(bg));
        return;
    }
    if (g_kwide[r][c] == 1) {                 /* CJK lead: 32x32 glyph over 2 cells */
        fb_fill(px, py, KCELLW * 2, KCELLH, kcon_vga(bg));
        const unsigned int *gl = kcon_cjk_glyph(g_kch[r][c]);
        if (gl) {
            for (int ry = 0; ry < 32; ry++) {
                unsigned bits = gl[ry];
                for (int rx = 0; rx < 32; rx++)
                    if (bits & (0x80000000u >> rx)) fb_set_px(px + rx, py + ry, kcon_vga(fg));
            }
        } else {
            fb_fill(px + 4, py + 4, KCELLW * 2 - 8, KCELLH - 8, kcon_vga(fg));
        }
        return;
    }
    /* ASCII: 8x16 font scaled 2x into the 16x32 cell */
    fb_fill(px, py, KCELLW, KCELLH, kcon_vga(bg));
    const unsigned char *g = SUN8x16 + (g_kch[r][c] & 0xFF) * 16;
    for (int ry = 0; ry < 16; ry++) {
        unsigned bits = g[ry];
        for (int rx = 0; rx < 8; rx++)
            if (bits & (0x80 >> rx)) fb_fill(px + rx * 2, py + ry * 2, 2, 2, kcon_vga(fg));
    }
}

static void kcon_refresh_all(void) {
    for (int r = 0; r < g_krows; r++) for (int c = 0; c < g_kcols; c++) kcon_draw_cell(c, r);
}

static void kcon_scroll(void) {
    for (int r = 1; r < g_krows; r++)
        for (int c = 0; c < g_kcols; c++) {
            g_kch[r-1][c] = g_kch[r][c];
            g_kat[r-1][c] = g_kat[r][c];
            g_kwide[r-1][c] = g_kwide[r][c];
        }
    int lr = g_krows - 1;
    for (int c = 0; c < g_kcols; c++) { g_kch[lr][c] = ' '; g_kat[lr][c] = KATTR(g_kfg,g_kbg); g_kwide[lr][c] = 0; }
    g_kcy = g_krows - 1; g_kcx = 0;
    kcon_refresh_all();
}

static void kcon_place_cursor(void) {
    if (g_kcx < g_kcols && g_kcy < g_krows)
        fb_fill(g_kcx * KCELLW, g_kcy * KCELLH + KCELLH - 4, KCELLW, 4, kcon_vga(g_kfg));
}

static void kcon_putc(UINT32 cp) {
    if (!g_kinit || !g_kcon_enable || gFbW == 0) return;
    kcon_draw_cell(g_kcx, g_kcy);   /* clear any cursor marker at current cell */
    if (cp == '\r') { g_kcx = 0; }
    else if (cp == '\n') {
        g_kcy++;
        if (g_kcy >= g_krows) { g_kcy = g_krows - 1; kcon_scroll(); }
        g_kcx = 0;
    }
    else if (cp == 0x08) {  /* backspace */
        if (g_kcx > 0) g_kcx--;
        g_kch[g_kcy][g_kcx] = ' '; g_kat[g_kcy][g_kcx] = KATTR(g_kfg,g_kbg); g_kwide[g_kcy][g_kcx] = 0;
        kcon_draw_cell(g_kcx, g_kcy);
    }
    else {
        /* Any non-ASCII codepoint is a wide (2-cell) glyph.  If it is not in
         * the font, kcon_cjk_glyph() returns 0 and kcon_draw_cell() paints a
         * clean tofu box instead of falling back to ASCII garbage. */
        int wide = (cp >= 0x80) ? 1 : 0;
        const unsigned int *gl = kcon_cjk_glyph(cp);
        if (wide) {
            if (g_kcx >= g_kcols - 1) {
                g_kcy++;
                if (g_kcy >= g_krows) { g_kcy = g_krows - 1; kcon_scroll(); }
                g_kcx = 0;
                kcon_draw_cell(g_kcx, g_kcy);
            }
            g_kch[g_kcy][g_kcx]    = (UINT16)cp; g_kat[g_kcy][g_kcx]    = KATTR(g_kfg,g_kbg); g_kwide[g_kcy][g_kcx]    = 1;
            if (g_kcx + 1 < g_kcols) {
                g_kch[g_kcy][g_kcx+1] = ' '; g_kat[g_kcy][g_kcx+1] = KATTR(g_kfg,g_kbg); g_kwide[g_kcy][g_kcx+1] = 2;
            }
            kcon_draw_cell(g_kcx, g_kcy);
            if (g_kcx + 1 < g_kcols) kcon_draw_cell(g_kcx + 1, g_kcy);
            g_kcx += 2;
            if (g_kcx >= g_kcols) { g_kcx = 0; g_kcy++; if (g_kcy >= g_krows) { g_kcy = g_krows - 1; kcon_scroll(); } }
        } else {
            if (g_kcx >= g_kcols) {
                g_kcy++;
                if (g_kcy >= g_krows) { g_kcy = g_krows - 1; kcon_scroll(); }
                g_kcx = 0;
                kcon_draw_cell(g_kcx, g_kcy);
            }
            g_kch[g_kcy][g_kcx]    = (UINT16)(cp & 0xFF); g_kat[g_kcy][g_kcx] = KATTR(g_kfg,g_kbg); g_kwide[g_kcy][g_kcx] = 0;
            kcon_draw_cell(g_kcx, g_kcy);
            g_kcx++;
            if (g_kcx >= g_kcols) { g_kcx = 0; g_kcy++; if (g_kcy >= g_krows) { g_kcy = g_krows - 1; kcon_scroll(); } }
        }
    }
    kcon_place_cursor();
    /* no present here; the caller (con_print / con_write_bytes) flushes once */
}

static void kcon_clear(void) {
    if (!g_kinit || gFbW == 0) return;
    fb_fill(0, 0, gFbW, gFbH, kcon_vga(g_kbg));
    for (int r = 0; r < g_krows; r++) for (int c = 0; c < g_kcols; c++) {
        g_kch[r][c] = ' '; g_kat[r][c] = KATTR(g_kfg,g_kbg); g_kwide[r][c] = 0;
    }
    g_kcx = 0; g_kcy = 0;
    kcon_present();
}

static void kcon_init(void) {
    if (gFbW == 0 || gFbH == 0) return;
    g_kcols = gFbW / KCELLW;
    g_krows = gFbH / KCELLH;
    if (g_kcols > 200) g_kcols = 200;
    if (g_krows > 63)  g_krows = 63;
    g_kfg = 7; g_kbg = 0;
    g_kinit = 1;
    kcon_clear();
}

static void kcon_present(void) { gop_present(); }
static int mouse_poll(int *out) {   /* out[0]=x out[1]=y out[2]=buttons out[3]=clicked
                                        * buttons/clicked are bitmasks:
                                        *   bit0 = LEFT  (primary / touch)
                                        *   bit1 = RIGHT (alternate)
                                        * LEFT stays on bit0 for backward compat. */
    int x=0,y=0,lbut=0,rbut=0,lclk=0,rclk=0;
    static int cached_l = 0, cached_r = 0;   /* last known button state across idle frames */
    static int cached_x = 0, cached_y = 0;   /* last known position across idle frames */
    if (gAbs) {
        EFI_ABSOLUTE_POINTER_STATE s;
        if (!EFI_ERROR(gAbs->GetState(gAbs, &s))) {
            UINT64 mx = s.CurrentX, my = s.CurrentY;
            if (gAbs->Mode->AbsoluteMaxX) mx = mx * gFbW / gAbs->Mode->AbsoluteMaxX;
            if (gAbs->Mode->AbsoluteMaxY) my = my * gFbH / gAbs->Mode->AbsoluteMaxY;
            x = (int)mx; y = (int)my;
            cached_x = x; cached_y = y;      /* remember position for idle frames */
            lbut = (s.ActiveButtons & 0x1) ? 1 : 0;   /* left  (TouchActive)  */
            rbut = (s.ActiveButtons & 0x2) ? 1 : 0;   /* right (AltActive)     */
            lclk = (gMousePrev ==0 && lbut==1) ? 1 : 0;  /* left  rising edge */
            rclk = (gMousePrevR==0 && rbut==1) ? 1 : 0;  /* right rising edge */
            gMousePrev  = lbut;
            gMousePrevR = rbut;
            cached_l = lbut; cached_r = rbut;        /* remember for idle frames */
        } else {
            /* No new pointer event this frame (usb-tablet is event-driven).
             * Report the last button state so that hold-to-drag (LMB/RMB held)
             * stays reliable; only the click edge is suppressed. Without this
             * cache, m[2] (buttons) drops to 0 on every idle frame and any
             * "press-and-hold to move a window / paint a shape" breaks.
             * Likewise report the last cached position so the cursor does not
             * snap to (0,0) on every idle frame (it would vanish / stop tracking). */
            x = cached_x; y = cached_y;
            lbut = cached_l; rbut = cached_r; lclk = 0; rclk = 0;
        }
    }
    out[0]=x; out[1]=y;
    out[2]= lbut | (rbut<<1);
    out[3]= lclk | (rclk<<1);
    return 0;
}

/* ===================================================================
 * Networking — VirtIO legacy-net (PCI) + ARP/IPv4/UDP/DHCP + DNS/TCP/HTTP
 *
 * Minimal from-scratch stack so userland can SYS_fetch() a URL. No TLS.
 * The guest reaches the internet through QEMU user-mode (slirp) NAT.
 * All packet buffers are static (.bss) which, under the kernel's 1:1
 * physical==virtual mapping, are valid DMA addresses for the device.
 * =================================================================== */
#define NW_MMIO_BASE    0x0A000000ULL    /* first virtio-mmio transport (QEMU arm "virt") */
#define NW_MMIO_STRIDE  0x200            /* 512 bytes per transport slot */
#define NW_MMIO_MAGIC   0x74726976U      /* "virt" */
#define NW_VIRTIO_NET   1                /* virtio device id: network card */

/* --- tiny libc (kernel is /nodefaultlib) --- */
static void nw_memset(void *d, int v, int n){ UINT8 *p=(UINT8*)d; while(n--) *p++=(UINT8)v; }
static void nw_memcpy(void *d, const void *s, int n){ UINT8 *p=(UINT8*)d; const UINT8 *q=(const UINT8*)s; while(n--) *p++=*q++; }
/* The firmware MMU stays enabled (hudos never calls ExitBootServices) and maps
 * normal memory as cacheable. VirtIO does DMA straight to physical RAM, so the
 * CPU cache must be flushed (clean) before handing a buffer to the device and
 * invalidated before the CPU reads a buffer the device wrote, otherwise the
 * device sees stale RAM and the CPU sees stale RX data. Without this, TX/RX
 * silently fail (the device reads a garbage avail-ring index). */
static void nw_dcache_range(void *p, int len){
    UINT8 *a=(UINT8*)(((UINTN)p)&~(UINTN)63);
    UINT8 *e=(UINT8*)(((UINTN)p)+(UINTN)len);
    for(; a<e; a+=64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static int  nw_strlen(const char *s){ int n=0; while(s[n]) n++; return n; }
static int  nw_strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
static int  nw_hexc(char c){ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }
/* internet checksum (one's complement, 16-bit) over len bytes (len may be odd) */
static UINT16 nw_cksum(const void *vp, int len){
    const UINT8 *p=(const UINT8*)vp; UINT32 sum=0; int i;
    for(i=0;i+1<len;i+=2) sum += ((UINT16)p[i]<<8)|p[i+1];
    if(len&1) sum += ((UINT16)p[len-1]<<8);
    while(sum>>16) sum = (sum&0xFFFF)+(sum>>16);
    return (UINT16)~sum;
}

/* byte-order helpers: host (little-endian aarch64) <-> network (big-endian).
 * Every multi-byte field placed on the wire MUST be converted htons/htonl;
 * every multi-byte field read off the wire MUST be ntohs/ntohl. This was the
 * core bug that silently broke DHCP/DNS/TCP — all fields were stored in
 * host order, so slirp received a port of 17152 (0x4300) instead of 67. */
static UINT16 htons(UINT16 v){ return (UINT16)(((v>>8)&0xFF)|((v&0xFF)<<8)); }
static UINT16 ntohs(UINT16 v){ return htons(v); }
static UINT32 htonl(UINT32 v){ return (UINT32)((v>>24)&0xFF | ((v>>16)&0xFF)<<8 | ((v>>8)&0xFF)<<16 | (v&0xFF)<<24); }
static UINT32 ntohl(UINT32 v){ return htonl(v); }

/* --- VirtIO MMIO transport (supports legacy v1 and modern v2) ---
 * Legacy v1 derives avail/used ring addresses from a single QueuePFN using a
 * layout QEMU computes; on QEMU 11 the guest driver must match that layout or
 * the device reads an empty avail ring and never pops. Modern v2 lets the guest
 * set QueueDescLow / QueueDriverLow / QueueDeviceLow explicitly, so the layout is
 * fully under guest control and always matches. We auto-detect the version and
 * prefer modern (set `force-legacy=off` on the QEMU device so v2 is presented). */
static UINT64 nw_mmio = 0;   /* selected transport MMIO base */
static int    g_nw_ver = 1;  /* 1 = legacy, 2 = modern */
static inline volatile UINT32* nw_m32(UINTN off){ return (volatile UINT32*)(UINTN)(nw_mmio+off); }
static inline volatile UINT8 * nw_m8 (UINTN off){ return (volatile UINT8 *)(UINTN)(nw_mmio+off); }

/* Register offsets are IDENTICAL in legacy (v1) and modern (v2) per the
 * authoritative QEMU header include/standard-headers/linux/virtio_mmio.h.
 * Only the queue-enable mechanism differs: v2 uses QueueReady (0x44) while
 * v1 uses QueuePFN (0x40), and v2 additionally exposes the 64-bit
 * desc/avail/used address registers at 0x80/0x90/0xa0. Everything else below
 * is shared, so we use the same constants for both versions. */
#define NW_QSEL    0x30   /* QueueSel */
#define NW_QNUMMAX 0x34   /* QueueNumMax */
#define NW_QNUM    0x38   /* QueueNum */
#define NW_QNOTIFY 0x50   /* QueueNotify */
#define NW_INTSTS  0x60   /* InterruptStatus */
#define NW_STATUS  0x70   /* Status */
#define NW_QREADY  (g_nw_ver==2 ? 0x44 : 0x40)   /* QueueReady (v2) / QueuePFN (v1 enable) */
/* modern v2 64-bit queue address registers (guest writes desc/avail/used explicitly) */
#define NW_QDESC_LO  0x80
#define NW_QDESC_HI  0x84
#define NW_QAVAIL_LO 0x90
#define NW_QAVAIL_HI 0x94
#define NW_QUSED_LO  0xA0
#define NW_QUSED_HI  0xA4
/* feature-select registers (shared offsets in v1/v2) */
#define NW_DEVFEAT_SEL  0x14   /* DeviceFeaturesSel */
#define NW_DEVFEAT      0x10   /* DeviceFeatures */
#define NW_DRVFEAT_SEL  0x24   /* DriverFeaturesSel (legacy: GuestFeaturesSel) */
#define NW_DRVFEAT      0x20   /* DriverFeatures (legacy: GuestFeatures) */

/* virtqueue layout (legacy): desc@0, avail@4096, used@8192 (matches Linux vring_init) */
struct nw_vdesc { UINT64 addr; UINT32 len; UINT16 flags; UINT16 next; };
struct nw_vused_elem { UINT32 id; UINT32 len; };
struct nw_vavail { UINT16 flags; UINT16 idx; UINT16 ring[256]; };
struct nw_vused  { UINT16 flags; UINT16 idx; struct nw_vused_elem ring[256]; };
/* virtio-net header size: 10 bytes in legacy (v1), 12 bytes in modern v1.1
 * (adds num_buffers). Must match the negotiated version. */
#define NW_VNET_HDR (g_nw_ver==2 ? 12 : 10)
#define VRING_DESC_F_WRITE 2  /* device writes into this descriptor (RX buffers) */
#define NW_TXBUF_SZ 4096      /* g_txbuf is one 4K page; >sizeof(UINT8*) which is 8 */

/* vring + packet buffers live in statically-allocated, page-aligned .bss arrays.
 * The EFI image is loaded and runs under the firmware's 1:1 (VA==PA) identity
 * map, so the addresses the guest computes (QueuePFN, descriptor ring entries)
 * are exactly the physical addresses the virtio device DMA engine uses. Using
 * AllocatePages here instead produced non-identity-mapped VAs in this firmware,
 * so the device read the vring from the wrong physical page (garbage avail
 * index -> TX/RX never completed). */
static UINT8 g_rxq_buf[3*4096]   __attribute__((aligned(4096)));
static UINT8 g_txq_buf[3*4096]   __attribute__((aligned(4096)));
static UINT8 g_rxbufs_buf[4*4096] __attribute__((aligned(4096)));
static UINT8 g_txbuf_buf[4096]    __attribute__((aligned(4096)));
static UINT8 *g_rxq=g_rxq_buf, *g_txq=g_txq_buf, *g_rxbufs=g_rxbufs_buf, *g_txbuf=g_txbuf_buf;
static int    g_dma_inited=0;
static int   g_rx_num=0, g_tx_num=0;
static UINT16 g_rx_used_idx=0, g_tx_used_idx=0;

/* DMA regions are static .bss arrays (see declarations above), so nothing to
 * allocate at runtime. Kept as a no-op hook for clarity / future use. */
static int nw_alloc_dma(void){
    g_dma_inited=1;
    return 0;
}

static UINT8  g_net_mac[6];
static int    g_net_ready=0;
static UINT32 g_our_ip=0, g_gw_ip=0, g_dns_ip=0, g_mask=0;
static UINT8  g_gw_mac[6];

/* debug counters (for the 421 self-test) */
static int g_dbg_rx_total=0, g_dbg_rx_udp68=0, g_dbg_offer=0, g_dbg_tx_done=0;

/* ARP cache (just the gateway) */
static void nw_eth_send(const UINT8 *dst, UINT16 et, const UINT8 *pl, int len);

/* ---- VirtIO device setup (version aware) ---- */
static int nw_vq_setup(UINT8 *q, int qidx, int *num){
    *nw_m32(NW_QSEL) = (UINT32)qidx;          /* QueueSel */
    UINT32 sz = *nw_m32(NW_QNUMMAX);          /* QueueNumMax */
    if(sz==0) return -1;
    if(sz>256) sz=256;                         /* cap to our descriptor-table size */
    *num = (int)sz;
    nw_memset(q,0,3*4096);
    if(g_nw_ver==2){
        /* modern v2: guest sets desc/driver/device addresses explicitly.
         * layout within the 3-page block: desc@0, avail@4096, used@8192. */
        *nw_m32(NW_QNUM) = (UINT32)sz;            /* QueueNum (0x38) */
        *nw_m32(NW_QDESC_LO)  = (UINT32)(UINTN)q;         /* QueueDescLow */
        *nw_m32(NW_QDESC_HI)  = 0;                        /* QueueDescHigh */
        *nw_m32(NW_QAVAIL_LO) = (UINT32)(UINTN)(q+4096);  /* QueueAvailLow (avail ring) */
        *nw_m32(NW_QAVAIL_HI) = 0;                        /* QueueAvailHigh */
        *nw_m32(NW_QUSED_LO)  = (UINT32)(UINTN)(q+8192);  /* QueueUsedLow (used ring) */
        *nw_m32(NW_QUSED_HI)  = 0;                        /* QueueUsedHigh */
        *nw_m32(NW_QREADY) = 1;                   /* QueueReady (0x44) */
    } else {
        /* legacy v1: single PFN; QEMU derives avail/used from it. */
        *nw_m32(NW_QNUM) = (UINT32)sz;            /* QueueNum */
        *nw_m32(0x3C) = 4096;                     /* QueueAlign (page size) */
        UINT64 pfn = ((UINT64)(UINTN)q) >> 12;
        *nw_m32(0x40) = (UINT32)pfn;              /* QueuePFN (legacy enable) */
    }
    /* flush desc+avail+used rings so the device sees our setup writes */
    nw_dcache_range(q, 3*4096);
    return 0;
}

static int nw_detect(void){
    /* scan the virtio-mmio transports for a network device (legacy v1 or modern v2) */
    for(int i=0; i<32; i++){
        UINT64 base = NW_MMIO_BASE + (UINT64)i*NW_MMIO_STRIDE;
        volatile UINT32 *m = (volatile UINT32*)(UINTN)base;
        if(m[0] != NW_MMIO_MAGIC) continue;            /* MagicValue */
        UINT32 ver   = m[1];                           /* Version */
        UINT32 devid = m[2];                           /* DeviceID */
        if(ver != 1 && ver != 2) continue;            /* legacy(1) or modern(2) only */
        if(devid == (UINT32)NW_VIRTIO_NET){            /* network card */
            nw_mmio = base;
            g_nw_ver = (int)ver;
            return i;
        }
    }
    return -1;
}

static int nw_virtio_init(void){
    int idx = nw_detect();
    if(idx<0) return -1;
    if(nw_alloc_dma()) return -1;
    /* reset + feature negotiation */
    *nw_m32(NW_STATUS) = 0;                   /* status = 0 (reset) */
    *nw_m32(NW_STATUS) = 1;                   /* ACKNOWLEDGE */
    *nw_m32(NW_STATUS) = 3;                   /* ACKNOWLEDGE|DRIVER */
    if(g_nw_ver==2){
        /* modern v2: read device features, then ACCEPT VIRTIO_F_VERSION_1
         * (bit 32) which is mandatory for the device to enter modern mode.
         * We accept only VERSION_1 (no GSO/MRG_RXBUF/etc.) so the 12-byte
         * v1.1 header and simple descriptor model apply. */
        *nw_m32(NW_DEVFEAT_SEL) = 0; (void)*nw_m32(NW_DEVFEAT);  /* sel 0, read */
        *nw_m32(NW_DEVFEAT_SEL) = 1; (void)*nw_m32(NW_DEVFEAT);  /* sel 1, read (VERSION_1) */
        *nw_m32(NW_DRVFEAT_SEL) = 1;          /* select high feature word */
        *nw_m32(NW_DRVFEAT)     = (UINT32)(1u << 0);  /* accept bit 32 = VIRTIO_F_VERSION_1 */
        *nw_m32(NW_DRVFEAT_SEL) = 0;          /* select low feature word */
        *nw_m32(NW_DRVFEAT)     = (UINT32)(1u << 5);  /* accept bit 5 = VIRTIO_NET_F_MAC
                                                       * (without this the net config space does NOT
                                                       * expose the MAC at offset 0, so our read
                                                       * would grab status/mtu bytes instead) */
    } else {
        *nw_m32(NW_DRVFEAT_SEL) = 0;          /* GuestFeaturesSel = 0 */
        *nw_m32(NW_DRVFEAT)     = 0;          /* negotiate no features (legacy) */
    }
    *nw_m32(NW_STATUS) = 0x0B;                /* ACK|DRIVER|FEATURES_OK */
    /* NOTE: virtqueues MUST be configured BEFORE DRIVER_OK, otherwise the
     * device ignores the queue setup and the rings stay disabled (no TX/RX). */
    if(nw_vq_setup(g_rxq,0,&g_rx_num)) return -1;
    if(nw_vq_setup(g_txq,1,&g_tx_num)) return -1;
    *nw_m32(NW_STATUS) = 0x0F;                /* ...|DRIVER_OK */
    /* read MAC (net config at 0x100) */
    for(int i=0;i<6;i++) g_net_mac[i]=*nw_m8(0x100+i);
    /* Bring the link up (QEMU virtio-net gates receive on LINK_UP). Harmless in v2. */
    *nw_m8(0x106) = 1;                        /* status = VIRTIO_NET_S_LINK_UP */
    /* Post RX buffers and kick the RX queue AFTER DRIVER_OK. The device only
     * honors queue notifications once DRIVER_OK is set, so a kick issued
     * earlier (before DRIVER_OK) is silently ignored and the device would
     * never pop the RX avail ring (TX worked only because its kick happens
     * after DRIVER_OK). */
    for(int i=0;i<8;i++){
        struct nw_vdesc *d=(struct nw_vdesc*)g_rxq;
        d[i].addr=(UINT64)(UINTN)(g_rxbufs+(UINTN)i*1600); d[i].len=1600; d[i].flags=VRING_DESC_F_WRITE; d[i].next=0;
        struct nw_vavail *av=(struct nw_vavail*)(g_rxq+4096);
        av->ring[i]=(UINT16)i;
    }
    struct nw_vavail *av=(struct nw_vavail*)(g_rxq+4096);
    av->idx=8; g_rx_used_idx=0;
    *nw_m32(NW_QNOTIFY) = 0;                  /* notify rx queue */
    return 0;
}

/* ---- raw frame tx (prepends virtio_net_hdr) ---- */
static int nw_send_raw(const UINT8 *frame, int len){
    if(len+NW_VNET_HDR> (int)NW_TXBUF_SZ) return -1;
    nw_memset(g_txbuf,0,NW_VNET_HDR);
    nw_memcpy(g_txbuf+NW_VNET_HDR, frame, len);
    struct nw_vdesc *d=(struct nw_vdesc*)g_txq;
    d[0].addr=(UINT64)(UINTN)g_txbuf; d[0].len=len+NW_VNET_HDR; d[0].flags=0; d[0].next=0;
    struct nw_vavail *av=(struct nw_vavail*)(g_txq+4096);
    /* VirtIO requires the driver to *increment* avail.idx for every new
     * descriptor posted. We always reuse desc 0 (safe because we block below
     * until the device marks it used before returning). */
    av->ring[av->idx & 255]=0; av->idx++;
    /* flush descriptor table + avail ring, and the payload buffer, so the
     * device DMA-reads our latest writes (aarch64 cache coherency) */
    nw_dcache_range(g_txq, 3*4096);
    nw_dcache_range(g_txbuf, len+NW_VNET_HDR);
    *nw_m32(NW_QNOTIFY) = 1;                  /* notify tx queue (vq index 1) */
    /* wait for tx used (device advances used idx by exactly 1 per send) */
    struct nw_vused *u=(struct nw_vused*)(g_txq+8192);
    int waited=0;
    while(waited<4000){
        nw_dcache_range(g_txq+8192, 4096);    /* invalidate used ring (device wrote it) */
        if(u->idx != g_tx_used_idx) break;
        if(gBS) gBS->Stall(1000); waited++;
    }
    if(u->idx!=g_tx_used_idx) g_dbg_tx_done++;
    g_tx_used_idx = u->idx;
    return 0;
}

/* ---- ethernet / arp / ip helpers ---- */
static UINT8 g_rxeth[1600]; static int g_rxeth_len=0;
static void nw_eth_broadcast(UINT8 *m){ for(int i=0;i<6;i++) m[i]=0xFF; }

static void nw_eth_send(const UINT8 *dst, UINT16 et, const UINT8 *pl, int len){
    UINT8 f[1600]; int o=0;
    nw_memcpy(f,dst,6); o+=6;
    nw_memcpy(f+6,g_net_mac,6); o+=6;
    f[12]=(et>>8)&0xFF; f[13]=et&0xFF; o+=2;
    nw_memcpy(f+14,pl,len); o+=len;
    nw_send_raw(f,o);
}

static void nw_arp_request(UINT32 tip){
    UINT8 p[28]; int o=0;
    *(UINT16*)(p+o)=htons(0x0001); o+=2;     /* hw type ethernet */
    *(UINT16*)(p+o)=htons(0x0800); o+=2;     /* proto IPv4 */
    p[o++]=6; p[o++]=4;                       /* hwlen, protolen */
    *(UINT16*)(p+o)=htons(0x0001); o+=2;      /* opcode request */
    nw_memcpy(p+o,g_net_mac,6); o+=6;         /* sha */
    *(UINT32*)(p+o)=htonl(g_our_ip); o+=4;   /* spa */
    nw_memset(p+o,0,6); o+=6;                 /* tha */
    *(UINT32*)(p+o)=htonl(tip); o+=4;         /* tpa */
    UINT8 b[6]; nw_eth_broadcast(b);
    nw_eth_send(b,0x0806,p,28);
}
static void nw_arp_reply(const UINT8 *req, int len){
    if(len<28) return;
    UINT8 p[28]; int o=0;
    *(UINT16*)(p+o)=htons(0x0001); o+=2;
    *(UINT16*)(p+o)=htons(0x0800); o+=2;
    p[o++]=6; p[o++]=4;
    *(UINT16*)(p+o)=htons(0x0002); o+=2;     /* reply */
    nw_memcpy(p+o,g_net_mac,6); o+=6;
    *(UINT32*)(p+o)=htonl(g_our_ip); o+=4;
    nw_memcpy(p+o,req+8,6); o+=6;            /* tha = sender ha (ARP offset 8) */
    nw_memcpy(p+o,req+14,4); o+=4;           /* tpa = sender pa (network order bytes) */
    UINT8 dst[6]; nw_memcpy(dst,req+6,6);
    nw_eth_send(dst,0x0806,p,28);
}

/* IPv4 send. gw = gateway MAC (or broadcast for DHCP). */
static void nw_ip_send(UINT32 dstip, const UINT8 *gwmac, UINT8 proto, const UINT8 *pl, int len){
    UINT8 ip[1600]; int o=0;   /* 20-byte header + up to ~1500-byte payload */
    ip[o++]=0x45; ip[o++]=0;                 /* v4, IHL5, tos */
    *(UINT16*)(ip+o)=htons((UINT16)(20+len)); o+=2; /* total len */
    *(UINT16*)(ip+o)=0; o+=2; *(UINT16*)(ip+o)=0; o+=2; /* id, frag */
    ip[o++]=64; ip[o++]=proto;               /* ttl, proto */
    *(UINT16*)(ip+o)=0; o+=2;                /* checksum (filled later) */
    *(UINT32*)(ip+o)=htonl(g_our_ip); o+=4;
    *(UINT32*)(ip+o)=htonl(dstip); o+=4;
    *(UINT16*)(ip+10)=htons(nw_cksum(ip,20));
    nw_memcpy(ip+20, pl, len);                /* copy L4 payload after the 20-byte IP header */
    nw_eth_send(gwmac,0x0800,ip,20+len);
}

/* UDP send */
static void nw_udp_send(UINT32 dstip, const UINT8 *gwmac, UINT16 sport, UINT16 dport, const UINT8 *pl, int len){
    UINT8 udp[8+1500]; int o=0;
    *(UINT16*)(udp+o)=htons(sport); o+=2;
    *(UINT16*)(udp+o)=htons(dport); o+=2;
    *(UINT16*)(udp+o)=htons((UINT16)(8+len)); o+=2;
    *(UINT16*)(udp+o)=0; o+=2;               /* checksum (0 ok for IPv4 UDP) */
    nw_memcpy(udp+8,pl,len); o+=len;
    nw_ip_send(dstip, gwmac, 17, udp, 8+len);
}

/* ---- receive dispatch: returns 1 if a relevant IP packet was handled ---- */
/* pending UDP / TCP delivery */
static UINT16 g_udp_lport=0; static UINT8 g_udp_rx[1500]; static int g_udp_rx_len=0; static int g_udp_rx_rdy=0;
static UINT8 g_udp_src[4];

/* process one received ethernet frame */
static int nw_frame_dispatch(const UINT8 *f, int len){
    if(len<14) return 0;
    UINT16 et = ((UINT16)f[12]<<8)|f[13];
    if(et==0x0806){ /* ARP */
        UINT16 op = ((UINT16)f[14+6]<<8)|f[14+7];
        if(op==1){ /* who-has */
            UINT32 tpa = ntohl(*(UINT32*)(f+14+24));
            if(tpa==g_our_ip) nw_arp_reply(f+14,28);
        } else if(op==2){ /* reply */
            UINT32 spa = ntohl(*(UINT32*)(f+14+14));
            if(spa==g_gw_ip) nw_memcpy(g_gw_mac,f+14+8,6);  /* SHA at ARP offset 8 */
        }
        return 0;
    }
    if(et!=0x0800) return 0;
    const UINT8 *ip = f+14;
    if(len<14+20) return 0;
    UINT8 proto = ip[9];
    int ihl = (ip[0]&0xF)*4;
    const UINT8 *l4 = ip+ihl;
    int l4len = ((UINT16)ip[2]<<8|ip[3]) - ihl;
        if(proto==17){ /* UDP */
        UINT16 dport = ((UINT16)l4[2]<<8)|l4[3];
        if(dport==68) g_dbg_rx_udp68++;
        if(dport==g_udp_lport && l4len>=8){
            int plen = ((UINT16)l4[4]<<8|l4[5]) - 8;
            if(plen>sizeof(g_udp_rx)) plen=sizeof(g_udp_rx);
            nw_memcpy(g_udp_rx, l4+8, plen);
            g_udp_rx_len=plen; nw_memcpy(g_udp_src,(UINT8*)(ip+12),4); /* keep network-order bytes */
            g_udp_rx_rdy=1;
            return 1;
        }
        return 0;
    }
    if(proto==6){ /* TCP — handled by caller via nw_tcp_input */
        return 2; /* signal TCP packet present */
    }
    return 0;
}

/* poll rx queue; copy one eth frame into g_rxeth; returns len or 0 */
static int nw_rx_poll(void){
    struct nw_vused *u=(struct nw_vused*)(g_rxq+8192);
    nw_dcache_range(g_rxq+8192, 4096);    /* invalidate used ring (device wrote it) */
    if(u->idx==g_rx_used_idx) return 0;
    struct nw_vused_elem *e = &u->ring[g_rx_used_idx & 255];
    UINT32 id = e->id; UINT32 plen = e->len;
    nw_dcache_range(g_rxbufs+(UINTN)id*1600, 1600);  /* invalidate RX buffer (device wrote it) */
    UINT8 *buf = g_rxbufs+(UINTN)id*1600;
    int flen = (int)plen - NW_VNET_HDR;
    if(flen<0) flen=0; if(flen>sizeof(g_rxeth)) flen=sizeof(g_rxeth);
    nw_memcpy(g_rxeth, buf+NW_VNET_HDR, flen);
    g_dbg_rx_total++;
    /* repost this descriptor */
    struct nw_vdesc *d=(struct nw_vdesc*)g_rxq;
    d[id].addr=(UINT64)(UINTN)(g_rxbufs+(UINTN)id*1600); d[id].len=1600; d[id].flags=VRING_DESC_F_WRITE; d[id].next=0;
    struct nw_vavail *av=(struct nw_vavail*)(g_rxq+4096);
    av->ring[av->idx & 255]=(UINT16)id; av->idx++;
    *nw_m32(NW_QNOTIFY)=0;
    nw_dcache_range(g_rxq, 3*4096);    /* flush reposted desc+avail */
    g_rx_used_idx++;
    return flen;
}

/* --- DHCP --- */
static UINT32 nw_dhcp_xid=0x12345678;
static int nw_dhcp(void){
    UINT8 disc[300]; int o=0;
    nw_memset(disc,0,sizeof(disc));
    disc[0]=1; disc[1]=1; disc[2]=6;          /* bootrequest, ethernet, hlen */
    *(UINT32*)(disc+4)=htonl(nw_dhcp_xid);
    *(UINT16*)(disc+10)=htons(0x8000);        /* broadcast flag */
    nw_memcpy(disc+28,g_net_mac,6);           /* chaddr */
    *(UINT32*)(disc+236)=htonl(0x63825363);   /* magic */
    disc[240]=53; disc[241]=1; disc[242]=1;   /* msg type: discover */
    disc[243]=55; disc[244]=4; disc[245]=1; disc[246]=3; disc[247]=6; disc[248]=51; /* params: mask,router,dns,lease */
    disc[249]=255;                            /* end */
    UINT8 b[6]; nw_eth_broadcast(b);
    g_udp_lport=68; g_udp_rx_rdy=0;
    nw_udp_send(0xFFFFFFFF, b, 68, 67, disc, 250);
    /* wait for offer (msg type 2) */
    UINT32 t0=0; (void)t0;
    for(int i=0;i<3000;i++){
        int l=nw_rx_poll();
        while(l>0){ if(nw_frame_dispatch(g_rxeth,l)==1){
            /* udp packet is DHCP: check xid + msg type */
            if(g_udp_rx_len>=244 && ntohl(*(UINT32*)(g_udp_rx+4))==nw_dhcp_xid && g_udp_rx[240]==53 && g_udp_rx[242]==2){
                g_dbg_offer=1;
                UINT32 offer=ntohl(*(UINT32*)(g_udp_rx+16));
                /* request */
                UINT8 req[300]; nw_memset(req,0,sizeof(req));
                req[0]=1; req[1]=1; req[2]=6;
                *(UINT32*)(req+4)=htonl(nw_dhcp_xid);
                *(UINT16*)(req+10)=htons(0x8000);
                nw_memcpy(req+28,g_net_mac,6);
                *(UINT32*)(req+236)=htonl(0x63825363);
                req[240]=53; req[241]=1; req[242]=3; /* request */
                req[243]=50; req[244]=4; *(UINT32*)(req+245)=htonl(offer); /* requested ip */
                req[249]=54; req[250]=4; nw_memcpy(req+251,g_udp_rx+20,4); /* server id (network order) */
                req[255]=255;
                g_udp_rx_rdy=0;
                nw_udp_send(0xFFFFFFFF,b,68,67,req,256);
                goto nw_dhcp_ack;
            }
        } l=nw_rx_poll(); }
        if(gBS) gBS->Stall(1000);
    }
    return -1;
nw_dhcp_ack:
    for(int i=0;i<3000;i++){
        int l=nw_rx_poll();
        while(l>0){ if(nw_frame_dispatch(g_rxeth,l)==1){
            if(g_udp_rx_len>=244 && ntohl(*(UINT32*)(g_udp_rx+4))==nw_dhcp_xid && g_udp_rx[240]==53 && (g_udp_rx[242]==5||g_udp_rx[242]==6)){
                g_our_ip=ntohl(*(UINT32*)(g_udp_rx+16));
                /* parse options */
                int p=240;
                while(p+1<g_udp_rx_len){
                    UINT8 code=g_udp_rx[p]; if(code==0){p++;continue;} if(code==255) break;
                    UINT8 ol=g_udp_rx[p+1]; int base=p+2;
                    if(code==1&&ol>=4) g_mask=ntohl(*(UINT32*)(g_udp_rx+base));
                    else if(code==3&&ol>=4) g_gw_ip=ntohl(*(UINT32*)(g_udp_rx+base));
                    else if(code==6&&ol>=4) g_dns_ip=ntohl(*(UINT32*)(g_udp_rx+base));
                    p=base+ol;
                }
                return 0;
            }
        } l=nw_rx_poll(); }
        if(gBS) gBS->Stall(1000);
    }
    return -1;
}

/* --- DNS A-record resolve --- */
static int nw_dns_resolve(const char *name, UINT32 *out){
    if(g_dns_ip==0) return -1;
    UINT8 q[300]; nw_memset(q,0,sizeof(q)); int o=0;
    UINT16 id=0x4242;
    *(UINT16*)(q+o)=htons(id); o+=2; *(UINT16*)(q+o)=htons(0x0100); o+=2; /* std query, recursion */
    *(UINT16*)(q+o)=htons(1); o+=2;                                       /* qdcount = 1 */
    *(UINT16*)(q+o)=htons(0); o+=2;                                       /* ancount = 0 */
    *(UINT16*)(q+o)=htons(0); o+=2;                                       /* nscount = 0 */
    *(UINT16*)(q+o)=htons(0); o+=2;                                       /* arcount = 0 (12-byte header) */
    /* question: name */
    const char *s=name;
    while(*s){
        const char *dot=s; while(*dot&&*dot!='.') dot++;
        int lab=(int)(dot-s); q[o++]=lab; nw_memcpy(q+o,s,lab); o+=lab;
        s=(*dot)?dot+1:dot;
    }
    q[o++]=0;
    *(UINT16*)(q+o)=htons(1); o+=2;  /* type A */
    *(UINT16*)(q+o)=htons(1); o+=2;  /* class IN */
    UINT16 sport=0xC000|((id)&0xFF);
    g_udp_lport=sport; g_udp_rx_rdy=0;
    UINT8 gw[6]; nw_memcpy(gw,g_gw_mac,6);
    nw_udp_send(g_dns_ip,gw,sport,53,q,o);
    for(int i=0;i<3000;i++){
        int l=nw_rx_poll();
        while(l>0){ if(nw_frame_dispatch(g_rxeth,l)==1){
            if(g_udp_rx_len>=12 && ntohs(*(UINT16*)(g_udp_rx))==id){
                /* parse answers */
                int p=12; /* skip header */
                /* skip questions */
                int qd=((UINT16)g_udp_rx[4]<<8)|g_udp_rx[5];
                for(int qi=0;qi<qd;qi++){
                    while(p<g_udp_rx_len && g_udp_rx[p]){ int lab=g_udp_rx[p++]; p+=lab; }
                    p++; if(p+4>g_udp_rx_len) break; p+=4;
                }
                int an=((UINT16)g_udp_rx[6]<<8)|g_udp_rx[7];
                for(int ai=0;ai<an;ai++){
                    /* name may be pointer */
                    if(p<g_udp_rx_len && (g_udp_rx[p]&0xC0)==0xC0) p+=2;
                    else { while(p<g_udp_rx_len && g_udp_rx[p]){ int lab=g_udp_rx[p++]; p+=lab; } p++; }
                    if(p+10>g_udp_rx_len) break;
                    UINT16 type=((UINT16)g_udp_rx[p]<<8)|g_udp_rx[p+1];
                    UINT16 rdlen=((UINT16)g_udp_rx[p+8]<<8)|g_udp_rx[p+9];
                    p+=10;
                    if(type==1 && rdlen==4){ *out=ntohl(*(UINT32*)(g_udp_rx+p)); return 0; }
                    p+=rdlen;
                }
                return -1;
            }
        } l=nw_rx_poll(); }
        if(gBS) gBS->Stall(1000);
    }
    return -1;
}

/* --- TCP (minimal client) --- */
struct nw_tcb {
    UINT32 rip; UINT16 rport; UINT16 lport;
    UINT32 seq; UINT32 ack;       /* our seq, our ack(of theirs) */
    int state;                    /* 0 closed,1 syn-sent,2 estab,3 fin */
    UINT8 gw[6];
};
static struct nw_tcb g_tcb;
static UINT8 g_tcp_rx[262144]; static int g_tcp_rx_len=0; static int g_tcp_rdy=0; static int g_tcp_fin=0;

static void nw_tcp_send(UINT8 flags, const UINT8 *pl, int len){
    UINT8 seg[20+1500]; int o=0;
    *(UINT16*)(seg+o)=htons(g_tcb.lport); o+=2;
    *(UINT16*)(seg+o)=htons(g_tcb.rport); o+=2;
    *(UINT32*)(seg+o)=htonl(g_tcb.seq); o+=4;
    *(UINT32*)(seg+o)=htonl(g_tcb.ack); o+=4;
    seg[o++]=5<<4; seg[o++]=flags;       /* data offset, flags */
    *(UINT16*)(seg+o)=htons(65535); o+=2; /* window */
    *(UINT16*)(seg+o)=0; o+=2;            /* urgent */
    /* checksum */
    UINT8 pb[20+1500+12]; int po=0;
    { UINT32 _s=htonl(g_our_ip); nw_memcpy(pb+po,&_s,4); po+=4; }
    { UINT32 _d=htonl(g_tcb.rip); nw_memcpy(pb+po,&_d,4); po+=4; }
    pb[po++]=0; pb[po++]=6;              /* zero, proto TCP */
    *(UINT16*)(pb+po)=htons((UINT16)(20+len)); po+=2;
    nw_memcpy(pb+po,seg,20); po+=20;
    if(len) nw_memcpy(pb+po,pl,len); po+=len;
    *(UINT16*)(seg+16)=htons(nw_cksum(pb,po));
    /* copy the payload into seg so the transmitted segment carries the data
     * (the checksum above already used pl; the wire copy must too). */
    if(len) nw_memcpy(seg+20,pl,len);
    nw_ip_send(g_tcb.rip,g_tcb.gw,6,seg,20+len);
}

static int nw_tcp_input(const UINT8 *eth, int len){
    const UINT8 *ip=eth+14; int ihl=(ip[0]&0xF)*4; const UINT8 *t=ip+ihl;
    if(((UINT16)t[0]<<8|t[1])!=g_tcb.rport) return 0;
    if(((UINT16)t[2]<<8|t[3])!=g_tcb.lport) return 0;
    UINT32 sseq=ntohl(*(UINT32*)(t+4)); UINT32 sack=ntohl(*(UINT32*)(t+8));
    UINT8 flags=t[13]; int plen=((UINT16)ip[2]<<8|ip[3])-ihl-20;
    int tcphdr=((t[12]>>4)&0xF)*4;   /* honor real TCP header length (servers send options) */
    const UINT8 *pl=t+tcphdr;
    if(flags&0x02){ /* SYN */
        g_tcb.ack = sseq+1;
        g_tcb.seq += 1; /* our SYN consumed one seq */
        nw_tcp_send(0x10,0,0); /* ACK */
        g_tcb.state=2;
        return 1;
    }
    if(g_tcb.state<2) return 0;
    /* data (first data segment carries seq == ack-1 because SYN consumed one) */
    if(plen>0){
        if(sseq==g_tcb.ack || sseq==g_tcb.ack-1){
            int room=sizeof(g_tcp_rx)-g_tcp_rx_len; if(room>plen) room=plen; if(room<0) room=0;
            nw_memcpy(g_tcp_rx+g_tcp_rx_len,pl,room); g_tcp_rx_len+=room;
            g_tcb.ack += plen;
            nw_tcp_send(0x10,0,0); /* ACK */
            g_tcp_rdy=1;
        }
        return 1;
    }
    if(flags&0x01){ /* FIN */
        g_tcb.ack += 1; g_tcp_fin=1; g_tcb.state=3;
        nw_tcp_send(0x11,0,0); /* FIN+ACK */
        return 1;
    }
    if(flags&0x10){ /* pure ACK */
        return 1;
    }
    return 0;
}

static int nw_tcp_connect(UINT32 rip, UINT16 rport){
    nw_memcpy(g_tcb.gw,g_gw_mac,6);
    g_tcb.rip=rip; g_tcb.rport=rport; g_tcb.lport=0xC100|((nw_dhcp_xid++)&0xFF);
    g_tcb.seq=0x1000; g_tcb.ack=0; g_tcb.state=1; g_tcb.seq+=0;
    g_tcp_rx_len=0; g_tcp_rdy=0; g_tcp_fin=0;
    nw_tcp_send(0x02,0,0); /* SYN */
    for(int i=0;i<4000;i++){
        int l=nw_rx_poll();
        while(l>0){ int r=nw_frame_dispatch(g_rxeth,l); if(r==2) nw_tcp_input(g_rxeth,l); l=nw_rx_poll(); }
        if(g_tcb.state==2) return 0;
        if(gBS) gBS->Stall(1000);
    }
    return -1;
}
static int nw_tcp_send_data(const UINT8 *d, int len){
    int sent=0;
    while(sent<len){
        int ch=len-sent; if(ch>1400) ch=1400;
        nw_tcp_send(0x18,d+sent,ch); /* PSH+ACK */
        g_tcb.seq += ch; sent+=ch;
        /* brief pause + drain ACKs */
        for(int i=0;i<30;i++){ int l=nw_rx_poll(); while(l>0){int r=nw_frame_dispatch(g_rxeth,l); if(r==2) nw_tcp_input(g_rxeth,l); l=nw_rx_poll();} if(gBS) gBS->Stall(10); }
    }
    return 0;
}
static int nw_tcp_recv(int timeout_ms){
    int waited=0;
    /* Accumulate ALL data segments until the peer sends FIN (or the buffer
     * fills, or we time out). Exiting on the first non-empty poll would drop
     * the rest of a multi-segment response. */
    while(!g_tcp_fin && g_tcp_rx_len < (int)sizeof(g_tcp_rx)-512 && waited<timeout_ms){
        int l=nw_rx_poll();
        while(l>0){ int r=nw_frame_dispatch(g_rxeth,l); if(r==2) nw_tcp_input(g_rxeth,l); l=nw_rx_poll(); }
        if(gBS) gBS->Stall(1000); waited+=1;
    }
    return g_tcp_rx_len;
}

/* --- TLS-friendly incremental TCP I/O ---------------------------------
 * The HTTP path (nw_tcp_recv) blocks until FIN/overflow. TLS keeps the
 * connection open, so we need byte-stream semantics: pump the NIC, peek how
 * many bytes are buffered, and read/consume exactly N without waiting for FIN. */
static void nw_tcp_pump(int ms){
    int w=0;
    while(w<ms){
        int l=nw_rx_poll();
        while(l>0){ int r=nw_frame_dispatch(g_rxeth,l); if(r==2) nw_tcp_input(g_rxeth,l); l=nw_rx_poll(); }
        if(gBS) gBS->Stall(1000);
        w++;
        if(g_tcp_rx_len>0) return;
    }
}
static int nw_tcp_avail(void){ return g_tcp_rx_len; }
static int nw_tcp_read(UINT8 *buf, int max){
    if(g_tcp_rx_len==0) nw_tcp_pump(3000);
    int n=g_tcp_rx_len; if(n>max) n=max; if(n<0) n=0;
    if(n>0){ nw_memcpy(buf,g_tcp_rx,n); nw_memcpy(g_tcp_rx,g_tcp_rx+n,g_tcp_rx_len-n); g_tcp_rx_len-=n; }
    return n;
}
static int nw_tcp_read_exact(UINT8 *buf, int n){
    int got=0;
    while(got<n){
        int r=nw_tcp_read(buf+got,n-got);
        if(r==0){ nw_tcp_pump(3000); r=nw_tcp_read(buf+got,n-got); if(r==0) return got; }
        got+=r;
    }
    return got;
}
static int nw_tcp_finned(void){ return g_tcp_fin; }

/* --- HTTP/1.0 GET (no TLS) --- */
static int nw_http_get(UINT32 ip, const char *host, const char *path, char *out, int cap, UINT16 port){
    if(nw_tcp_connect(ip,port)) return -1;
    /* build request */
    char req[2048]; int ro=0;
    const char *p="GET "; while(*p){req[ro++]=*p++;}
    p=path; while(*p){req[ro++]=*p++;}
    p=" HTTP/1.0\r\nHost: "; while(*p){req[ro++]=*p++;}
    p=host; while(*p){req[ro++]=*p++;}
    p="\r\n\r\n"; while(*p){req[ro++]=*p++;}
    nw_tcp_send_data((const UINT8*)req,ro);
    int n=nw_tcp_recv(60000);   /* generous timeout: local LLM may take >8s to answer */
    /* strip headers */
    int hdr_end=-1;
    for(int i=0;i+3<n;i++){ if(g_tcp_rx[i]=='\r'&&g_tcp_rx[i+1]=='\n'&&g_tcp_rx[i+2]=='\r'&&g_tcp_rx[i+3]=='\n'){ hdr_end=i+4; break; } }
    int body=0;
    if(hdr_end>=0) body=n-hdr_end; else body=n;
    if(body>cap) body=cap;
    if(body<0) body=0;
    nw_memcpy(out, g_tcp_rx + (hdr_end>=0?hdr_end:0), body);
    return body;
}

/* --- net_init (lazy, on first SYS_fetch) --- */
static int nw_init_done=0;
static int net_init(void){
    if(nw_init_done) return g_net_ready;
    nw_init_done=1;
    if(nw_virtio_init()) return -1;
    if(nw_dhcp()) return -1;
    /* ARP the gateway */
    if(g_gw_ip){
        nw_arp_request(g_gw_ip);
        for(int i=0;i<1500;i++){ int l=nw_rx_poll(); while(l>0){ nw_frame_dispatch(g_rxeth,l); l=nw_rx_poll(); } if(g_gw_mac[0]||g_gw_mac[1]) break; if(gBS) gBS->Stall(1000); }
    }
    g_net_ready=1;
    return 1;
}

/* forward decls (defined after sys_fetch below) */
static int sscanf_ip(const char *s, int *a, int *b, int *c, int *d);
static int nw_strncmp_url(const char *s, const char *pre, int n);

/* parse "http[s]://host[:port]/path" -> fill host[128], path[1024], *port
 * (default 80 / 443), *is_https (0/1).
 * return ip (IPv4 literal) or 0 (needs DNS) or -1 (bad scheme). */
static int nw_parse_url(const char *url, char *host, char *path, UINT32 *ip, UINT16 *port, int *is_https){
    *is_https=0;
    int off;
    if(nw_strncmp_url(url,"https://",8)==0){ *is_https=1; *port=443; off=8; }
    else if(nw_strncmp_url(url,"http://",7)==0){ *port=80; off=7; }
    else return -1;
    const char *p=url+off;
    const char *slash=p; while(*slash&&*slash!='/') slash++;
    const char *colon=p; while(*colon&&*colon!=':'&&*colon!='/') colon++;
    int hl=(int)(colon-p); if(hl>=128) hl=127; if(hl<0) hl=0; nw_memcpy(host,p,hl); host[hl]=0;
    if(*colon==':'){
        int pv=0; const char *q=colon+1;
        while(*q>='0'&&*q<='9'&&*q!='/'){ pv=pv*10+(*q-'0'); q++; }
        if(pv>0 && pv<65536) *port=(UINT16)pv;
    }
    if(*slash) nw_memcpy(path,slash,nw_strlen(slash)+1); else { path[0]='/'; path[1]=0; }
    /* is host an IPv4 literal? */
    int dots=0; for(int i=0;i<hl;i++) if(host[i]=='.') dots++;
    if(dots==3){
        int a=0,b=0,c=0,d=0; int got=sscanf_ip(host,&a,&b,&c,&d);
        if(got==4){ *ip=((UINT32)a<<24)|((UINT32)b<<16)|((UINT32)c<<8)|(UINT32)d; return 1; }
    }
    return 0;
}

/* ---- TLS client RNG (kernel side) ----
 * No libc RNG in the guest. Seed a ChaCha20 keystream DRBG from the UEFI
 * wall-clock time mixed with a few kernel state words, then expand with
 * ChaCha20. Adequate for a demo OS; NOT a FIPS-grade entropy source. */
static UINT8 g_tls_key[32];
static UINT8 g_tls_ctr[8];
static int   g_tls_rng_inited=0;

static void hudos_tls_rng_seed(void){
    UINT8 s[32]; nw_memset(s,0,32);
    if(gRT){
        EFI_TIME t;
        EFI_STATUS (*gt)(EFI_TIME *, void *) =
            (EFI_STATUS(*)(EFI_TIME *, void *))gRT->GetTime;
        if(gt && gt(&t, 0)==0){
            UINT32 v = (UINT32)t.Year*16777216u + (UINT32)t.Month*65536u
                     + (UINT32)t.Day*256u + (UINT32)t.Hour*16u
                     + (UINT32)t.Minute + (UINT32)t.Second;
            s[0]^=(UINT8)(v>>24); s[1]^=(UINT8)(v>>16); s[2]^=(UINT8)(v>>8); s[3]^=(UINT8)v;
            s[4]^=(UINT8)(t.Nanosecond>>24); s[5]^=(UINT8)(t.Nanosecond>>16);
            s[6]^=(UINT8)(t.Nanosecond>>8);  s[7]^=(UINT8)t.Nanosecond;
        }
    }
    UINT64 m = (UINT64)(UINTN)&g_tcb.seq ^ (UINT64)nw_dhcp_xid ^ (UINT64)(UINTN)s;
    for(int i=0;i<8;i++) s[8+i] ^= (UINT8)(m>>(8*i));
    sha256(s,32,g_tls_key);
    g_tls_ctr[0]=1;
    g_tls_rng_inited=1;
}
static int hudos_tls_rng(UINT8 *buf, int n){
    if(!g_tls_rng_inited) hudos_tls_rng_seed();
    UINT8 zero[64]; nw_memset(zero,0,64);
    int done=0;
    while(done<n){
        UINT8 nonce[12]; nw_memset(nonce,0,12);
        for(int i=0;i<8;i++) nonce[4+i]=g_tls_ctr[i];
        int ch=n-done; if(ch>64) ch=64;
        chacha20(g_tls_key, nonce, 0, zero, (UINT32)ch, buf+done);
        done+=ch;
        for(int i=7;i>=0;i--){ if(++g_tls_ctr[i]) break; }
    }
    return n;
}

/* ---- HTTPS fetch (TLS 1.2, no libc) ----
 * Transport callbacks bridge the TLS record layer to the kernel TCP stack. */
static int ks_send(void *ctx, const UINT8 *d, int n){
    (void)ctx;
    nw_tcp_send_data(d, n);   /* blocking; returns 0 */
    return n;
}
static int ks_recv(void *ctx, UINT8 *b, int m){
    (void)ctx;
    return nw_tcp_read(b, m);
}

static long sys_fetch_https(UINT32 ip, const char *host, const char *path,
                            char *buf, int cap, UINT16 port){
    if(nw_tcp_connect(ip,port)) return -1;
    tls_set_rng(&hudos_tls_rng);
    tls_conn c; c.send=ks_send; c.recv=ks_recv; c.ctx=0;
    tls_session s; nw_memset(&s,0,sizeof(s)); s.conn=c;
    /* trust store = embedded public root CAs */
    tls_trust trust; nw_memset(&trust,0,sizeof(trust));
    trust.n = TLS_ROOTS_N;
    for(int i=0;i<TLS_ROOTS_N;i++){ trust.certs[i]=g_tls_root_der[i]; trust.lens[i]=g_tls_root_len[i]; }
    if(tls_handshake(&s, host, &trust)!=0){ tls_close(&s); return -1; }
    /* HTTP/1.0 GET over the encrypted channel */
    char req[2048]; int ro=0;
    const char *p="GET "; while(*p){req[ro++]=*p++;}
    p=path; while(*p){req[ro++]=*p++;}
    p=" HTTP/1.0\r\nHost: "; while(*p){req[ro++]=*p++;}
    p=host; while(*p){req[ro++]=*p++;}
    p="\r\n\r\n"; while(*p){req[ro++]=*p++;}
    if(tls_send(&s,(const UINT8*)req,ro)<0){ tls_close(&s); return -1; }
    /* read application_data until the server closes (or buffer fills) */
    int total=0;
    for(;;){
        UINT8 chunk[16384];
        int n=tls_recv(&s, chunk, (int)sizeof(chunk));
        if(n<0) break;                 /* close_notify / EOF -> done */
        if(n==0){ if(gBS) gBS->Stall(2000); continue; }
        if(total+n>cap) n=cap-total;
        if(n>0){ nw_memcpy(buf+total, chunk, n); total+=n; }
        if(total>=cap) break;
    }
    tls_close(&s);
    /* strip HTTP headers */
    int hdr_end=-1;
    for(int i=0;i+3<total;i++){ if(buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n'){ hdr_end=i+4; break; } }
    int body=0;
    if(hdr_end>=0) body=total-hdr_end; else body=total;
    if(body<0) body=0; if(body>cap) body=cap;
    if(hdr_end>=0 && body>0) nw_memcpy(buf, buf+hdr_end, body);  /* forward copy (dst<src) is safe */
    return (long)body;
}

/* SYS_fetch(url, buf, cap): returns body length or -1 */
static long sys_fetch(const char *url, char *buf, int cap){
    if(net_init()!=1) return -1;
    char host[128], path[1024]; UINT32 ip=0; UINT16 port=80; int is_https=0;
    int isip = nw_parse_url(url,host,path,&ip,&port,&is_https);
    if(isip<0) return -1;
    if(isip!=1){ if(nw_dns_resolve(host,&ip)) return -1; }
    if(ip==0) return -1;
    if(is_https) return sys_fetch_https(ip,host,path,buf,cap,port);
    return (long)nw_http_get(ip,host,path,buf,cap,port);
}

/* helper: sscanf of "a.b.c.d" -> 4 ints (returns count) */
static int sscanf_ip(const char *s, int *a, int *b, int *c, int *d){
    int n=0; *a=*b=*c=*d=0; int cur=0; const char *p=s;
    while(*p){
        if(*p>='0'&&*p<='9'){ cur=cur*10+(*p-'0'); }
        else if(*p=='.'){ if(n==0)*a=cur; else if(n==1)*b=cur; else if(n==2)*c=cur; n++; cur=0; }
        p++;
    }
    if(n==3){ *d=cur; n++; }
    return n;
}
/* helper: case-insensitive match of url prefix */
static int nw_strncmp_url(const char *s, const char *pre, int n){ for(int i=0;i<n;i++){ if(s[i]!=pre[i]) return 1; } return 0; }

/* comprehensive net self-test: writes a human-readable report into ob and
 * returns the number of bytes written. Steps: detect -> virtio init -> dhcp.
 * NOTE: nw_dhcp is only attempted if nw_virtio_init succeeded, otherwise
 * touching the (unmapped) BAR0 would fault the kernel. */
static int nw_net_selftest(char *ob, int cap){
    int p=0;
    #define AP(x) do{ long v=(long)(x); if(v<0){ob[p++]='-';v=-v;} \
        if(v==0) ob[p++]='0'; else { char t[16]; int n=0; \
            while(v){ t[n++]=(char)('0'+(v%10)); v/=10; } \
            while(n--) ob[p++]=t[n]; } ob[p++]=' '; } while(0)
    #define APS(s) do{ const char *_s=(s); while(*_s && p<cap-2) ob[p++]=*_s++; ob[p++]=' '; } while(0)
    #define APIP(ip) do{ UINT32 _ip=(ip); AP((int)(_ip&0xFF)); AP((int)((_ip>>8)&0xFF)); \
        AP((int)((_ip>>16)&0xFF)); AP((int)((_ip>>24)&0xFF)); } while(0)

    APS("scan");
    for(int d=0; d<32; d++){
        UINT64 base = NW_MMIO_BASE + (UINT64)d*NW_MMIO_STRIDE;
        volatile UINT32 *m = (volatile UINT32*)(UINTN)base;
        if(m[0] != NW_MMIO_MAGIC) continue;
        AP(d); AP((int)m[1]); AP((int)m[2]);   /* idx, version, deviceid */
    }
    APS("detect"); int dev=nw_detect(); AP(dev); APS("ver"); AP(g_nw_ver);
    if(dev>=0){
        int r=nw_virtio_init(); APS("vio"); AP(r);
        if(r!=0){ APS("dmainit"); AP(g_dma_inited); *nw_m32(NW_QSEL)=0; APS("qnummax"); AP((int)*nw_m32(NW_QNUMMAX)); }
        APS("rxn"); AP(g_rx_num); APS("txn"); AP(g_tx_num);
        if(r==0){
            APS("mac"); for(int i=0;i<6;i++) AP((int)g_net_mac[i]);
            APS("cfg"); for(int i=0;i<96;i++) AP((int)*nw_m8(0x100+i));
            /* ---- DMA diagnostics ---- */
            APS("rxqalign"); AP((int)((UINTN)g_rxq & 0xFFF));
            APS("txqalign"); AP((int)((UINTN)g_txq & 0xFFF));
            *nw_m32(NW_QSEL)=0; APS("q0desc"); AP((int)*nw_m32(NW_QDESC_LO));
            *nw_m32(NW_QSEL)=1; APS("q1desc"); AP((int)*nw_m32(NW_QDESC_LO));
            *nw_m32(NW_QSEL)=1; APS("status"); AP((int)*nw_m32(NW_STATUS));
            /* manual TX, then watch InterruptStatus and tx used idx */
            g_tx_used_idx = ((struct nw_vused*)(g_txq+8192))->idx;
            { UINT8 f[60]; for(int i=0;i<60;i++) f[i]=0;
              for(int i=0;i<6;i++) f[i]=0xFF; nw_memcpy(f+6,g_net_mac,6);
              f[12]=0x08; f[13]=0x06;
              nw_send_raw(f,60); }
            UINT32 dints=0, dmaxused=0;
            for(int i=0;i<1500;i++){
                UINT32 is=*nw_m32(NW_INTSTS); if(is) dints|=is;
                UINT32 uidx=((struct nw_vused*)(g_txq+8192))->idx;
                if(uidx>dmaxused) dmaxused=uidx;
                if(gBS) gBS->Stall(1000);
            }
            APS("intsts"); AP((int)dints);
            APS("txused"); AP((int)dmaxused);
            /* ---- ARP probe: request gateway 10.0.2.2, poll 2s, see if RX delivers ---- */
            { UINT32 gwsav=g_gw_ip; nw_arp_request(0x0A000202);
              for(int i=0;i<2000;i++){ int l=nw_rx_poll(); while(l>0){ nw_frame_dispatch(g_rxeth,l); l=nw_rx_poll(); } if(gBS) gBS->Stall(1000); }
              APS("arpprobe_rxtot"); AP(g_dbg_rx_total); APS("arpprobe_gwmac");
              AP((int)g_gw_mac[0]); AP((int)g_gw_mac[1]); AP((int)g_gw_mac[2]);
              AP((int)g_gw_mac[3]); AP((int)g_gw_mac[4]); AP((int)g_gw_mac[5]);
              (void)gwsav; }
            /* ---- end DMA diagnostics ---- */
            int dh=nw_dhcp(); APS("dhcp"); AP(dh);
            APS("rxtot"); AP(g_dbg_rx_total); APS("udp68"); AP(g_dbg_rx_udp68); APS("offer"); AP(g_dbg_offer); APS("txdone"); AP(g_dbg_tx_done);
            APS("ip"); APIP(g_our_ip);
            APS("gw"); APIP(g_gw_ip);
            APS("dns"); APIP(g_dns_ip);
        }
    }
    #undef AP
    #undef APS
    #undef APIP
    if(p>cap-1) p=cap-1;
    ob[p]=0;
    return p;
}

/* =======================================================================
 * Intel HD Audio — minimal driver (QEMU ich9-intel-hda + hda-output)
 * Plays 44.1 kHz / 16-bit / MONO PCM via bus-master DMA. No interrupts are
 * used; completion is polled via the stream descriptor IOC status bit (bit26).
 * The kernel never calls ExitBootServices, so gBS + MMIO stay valid here.
 * All addresses are identity-mapped and there is no IOMMU on virt, so the
 * CPU physical address of a DMA buffer == the bus address the device uses.
 * ===================================================================== */

/* aarch64 cache maintenance (coherency for DMA buffers / CORB / RIRB). */
static void dcache_clean(const void *p, UINTN sz) {
    UINTN line = 64, a = (UINTN)p & ~(line-1), end = (UINTN)p + sz;
    for (; a < end; a += line) asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
}
static void dcache_inv(const void *p, UINTN sz) {
    UINTN line = 64, a = (UINTN)p & ~(line-1), end = (UINTN)p + sz;
    for (; a < end; a += line) asm volatile("dc ivac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

#define HDA_ECAM_BASE 0x4010000000ULL
#define HDA_CLS_AUDIO 0x0403

static uint32_t pci_cfg_r32(uint64_t ecam, int bus, int dev, int fn, int reg) {
    uint64_t addr = ecam + (((uint64_t)bus  & 0xff) << 20)
                        + (((uint64_t)dev  & 0x1f) << 15)
                        + (((uint64_t)fn   & 0x7)  << 12)
                        + (reg & 0xfff);
    return *(volatile uint32_t *)(UINTN)addr;
}
static void pci_cfg_w32(uint64_t ecam, int bus, int dev, int fn, int reg, uint32_t val) {
    uint64_t addr = ecam + (((uint64_t)bus  & 0xff) << 20)
                        + (((uint64_t)dev  & 0x1f) << 15)
                        + (((uint64_t)fn   & 0x7)  << 12)
                        + (reg & 0xfff);
    *(volatile uint32_t *)(UINTN)addr = val;
}

static volatile uint32_t *g_hda;     /* BAR0 mapping (global + SD regs) */
static inline uint32_t hwr(int o){ return g_hda[o/4]; }
static inline void      hww(int o, uint32_t v){ g_hda[o/4] = v; }
/* 1- and 2-byte MMIO accessors. Several HDA regs (CORBCTL, RIRBCTL, RIRBSTS,
 * RINTCNT, CORBRP, RIRBWP) are sub-32-bit; writing them with hww() would
 * clobber neighbouring registers. Use these for width-correct access. */
static inline uint8_t  hbrb(int o){ return ((volatile uint8_t *)g_hda)[o]; }
static inline void     hbwb(int o, uint8_t v){ ((volatile uint8_t *)g_hda)[o] = v; }
static inline void     hbw2(int o, uint16_t v){ ((volatile uint16_t *)g_hda)[o/2] = v; }

static uint8_t  *g_corb, *g_rirb;
static uint32_t  g_corb_wp;
static uint32_t  g_rirb_rp;
static int       g_hda_ok;
static int       g_hda_cad;
static uint32_t  g_hda_sd;           /* output SD4 MMIO base = 0x100 */
static uint32_t  g_hda_vid;          /* codec vendor id (diagnostics) */

/* HDA 12-bit verb encoding: (cad<<28)|(nid<<20)|(verb<<8)|payload */
/* HDA 12/4-bit verb encoding. QEMU (intel-hda.c) classifies by (data&0x70000):
 *   if verb high-nibble == 0x7 -> 12-bit verb + 8-bit payload (bits 7:0)
 *   else                       -> 4-bit verb + 16-bit payload (bits 15:0)
 * So the payload always occupies the low 16 bits; for 16-bit verbs the high
 * byte is meaningful. We therefore mask payload to 0xffff. */
#define HDA_VERB(cad,nid,verb,payload)                                  \
    ((uint32_t)(cad)<<28 | (uint32_t)(nid)<<20 |                       \
     ((uint32_t)(verb)&0xfff)<<8 | (uint32_t)(payload)&0xffff)

static void hda_corb_send(uint32_t cmd) {
    uint32_t wp = g_corb_wp & 0xff;
    ((volatile uint32_t *)g_corb)[wp] = cmd;
    dcache_clean(g_corb + wp*4, 4);
    g_corb_wp = (wp + 1) & 0xff;          /* next entry to write */
    hbw2(0x48, (uint16_t)wp);             /* CORBWP = last written entry (16-bit) */
}
static uint32_t hda_rirb_get(void) {
    uint32_t seen = g_rirb_rp & 0xff;
    int guard = 0;
    for (;;) {
        dcache_inv(g_rirb, 256*8);
        uint32_t wp = hwr(0x58) & 0xff;   /* RIRBWP = last written entry index */
        if (wp != seen) {
            uint32_t idx = wp;            /* QEMU writes the response at entry wp */
            /* RIRB entries are 8 bytes; the 32-bit response is the low word
             * of entry idx (i.e. uint32 word idx*2). */
            uint32_t resp = ((volatile uint32_t *)g_rirb)[idx*2];
            g_rirb_rp = wp;
            /* Clear RIRBSTS (response IRQ + overrun) at 0x5D (NOT 0x5E, which
             * is RIRBSIZE and is write-ignored in QEMU). Clearing RIRBSTS is
             * what makes QEMU reset d->rirb_count so corb_run can keep issuing
             * verbs; otherwise it jams after the gate. */
            hbwb(0x5D, 0x05);             /* clear RIRBSTS IRQ+OVERRUN */
            return resp;
        }
        if (++guard > 8000) return 0xFFFFFFFF;     /* ~80 ms timeout */
        gBS->Stall(10);
    }
}
static uint32_t hda_ic_verb(uint32_t verb);   /* forward decl (defined below) */
static uint32_t hda_verb(int cad, int nid, int verb, int payload) {
    /* Route all config verbs through the synchronous Immediate Command
     * interface (ICW/IRR/ICS @0x60/0x64/0x68). It is reliable and avoids the
     * CORB/RIRB ring read races we saw (RIRBWP advanced but responses read
     * back stale/timeout). The IC interface is sufficient for codec config;
     * actual PCM playback uses the output stream DMA, not verbs. */
    return hda_ic_verb(HDA_VERB(cad, nid, verb, payload));
}
/* ISOLATION PROBE: the Immediate Command (IC) interface issues a verb via the
 * ICW/IRR/ICS registers and bypasses the CORB/RIRB DMA ring entirely. QEMU
 * 11.1.0 defines: ICW=0x60 (verb in), IRR=0x64 (response out, read),
 * ICS=0x68 (bit0=BUSY issue, bit1=VALID done). Writing BUSY into ICS triggers
 * synchronous processing; QEMU then writes the codec response into IRR and sets
 * VALID, clearing BUSY. ics1: 0x02=VALID(cmd done), 0x01=BUSY stuck(find
 * failed), 0x00=BUSY never latched(IC not triggered). Does NOT clear ICS. */
static uint32_t hda_ic_verb(uint32_t verb) {
    hbw2(0x68, 0x02);                      /* clear stale VALID (wclear bit) */
    gBS->Stall(2);
    hww(0x60, verb);                       /* ICW: verb to issue */
    hbw2(0x68, 0x01);                      /* ICS: set BUSY @0x68 -> QEMU processes */
    int guard = 0;
    while (!(hwr(0x68) & 0x02) && guard++ < 8000)   /* poll VALID (bit1) @0x68 */
        gBS->Stall(10);
    uint32_t resp = hwr(0x64);             /* IRR @0x64: response */
    uint32_t ics1 = hwr(0x68);             /* final state @0x68 */
    hbw2(0x68, 0x02);                      /* clear VALID for the next command */
    if (guard >= 8000) return 0xFFFFFFFF | (ics1 << 8);
    return resp;
}
static void dbghex(uint32_t v);   /* forward decl; full def near cmd_sndtest */

/* probe + init the controller and codec. returns 0 on success, else -errno. */
static int hda_init(void) {
    if (g_hda_ok) return 0;
    uint64_t mmio = 0;
    /* full PCI bus-0 scan (diagnostic): show every device's vend:did + bar0 */
    for (int dev = 0; dev < 32; dev++) {
        uint32_t v = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0);
        if (v == 0xffffffff || v == 0) continue;
        uint32_t cc  = (pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x08) >> 16) & 0xffff;
        uint32_t bar0 = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x10);
        con_print_ascii("hda: pci dev="); dbghex((uint32_t)dev);
        con_print_ascii(" vend:did="); dbghex(v);
        con_print_ascii(" class="); dbghex(cc);
        con_print_ascii(" bar0="); dbghex(bar0); con_print_ascii("\r\n");
    }
    for (int dev = 0; dev < 32; dev++) {        /* scan PCI bus 0 */
        uint32_t v = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0);
        if (v == 0xffffffff || v == 0) continue;
        uint32_t cc  = (pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x08) >> 16) & 0xffff;
        uint32_t vend = v & 0xffff, did = (v >> 16) & 0xffff;
        if (cc == HDA_CLS_AUDIO ||
            (vend==0x8086 && did==0x293e)) {
            uint32_t bar0 = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x10);
            uint32_t bar1 = ((bar0 & 0x6) == 0x4)
                            ? pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x14) : 0;
            uint32_t cmdreg = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x04);
            con_print_ascii("hda: dev="); dbghex((uint32_t)dev);
            con_print_ascii(" vend="); dbghex(v);
            con_print_ascii(" bar0="); dbghex(bar0);
            con_print_ascii(" bar1="); dbghex(bar1);
            con_print_ascii(" cmd="); dbghex(cmdreg); con_print_ascii("\r\n");
            if ((bar0 & 0x1) == 0) {             /* MMIO BAR */
                mmio = (uint64_t)(bar0 & 0xFFFFFFF0ULL);
                if ((bar0 & 0x6) == 0x4)         /* 64-bit BAR */
                    mmio |= ((uint64_t)bar1) << 32;
            }
            /* enable Memory Space + Bus Master so the MMIO BAR responds */
            pci_cfg_w32(HDA_ECAM_BASE, 0, dev, 0, 0x04, cmdreg | 0x02 | 0x04);
            uint32_t cmd2 = pci_cfg_r32(HDA_ECAM_BASE, 0, dev, 0, 0x04);
            con_print_ascii("hda: cmd-after="); dbghex(cmd2); con_print_ascii("\r\n");
            break;
        }
    }
    if (!mmio) return -1;

    g_hda = (volatile uint32_t *)(UINTN)mmio;

    con_print_ascii("hda: GCAP="); dbghex(hwr(0x00));
    con_print_ascii(" GCTL@map="); dbghex(hwr(0x08));
    con_print_ascii(" STATESTS="); dbghex(hwr(0x0E)); con_print_ascii("\r\n");

    /* Standard HDA reset sequence: assert CRST (bit0=0), brief delay, then
     * de-assert (bit0=1) and poll until the controller reports ready.
     * NOTE: on QEMU writing CRST=0 triggers device_cold_reset, but the
     * controller's codec children are NOT detached by that (intel_hda_reset
     * only resets registers + sets STATESTS), so this is safe. */
    hww(0x08, hwr(0x08) & ~1u);          /* assert reset */
    gBS->Stall(200);
    hww(0x08, hwr(0x08) | 1);            /* de-assert reset (run) */
    int tries = 0;
    while (!(hwr(0x08) & 1) && tries++ < 2000) gBS->Stall(100);
    if (!(hwr(0x08) & 1)) return -2;
    con_print_ascii("hda: after-reset GCTL="); dbghex(hwr(0x08));
    con_print_ascii(" STATESTS="); dbghex(hwr(0x0E)); con_print_ascii("\r\n");

    /* allocate CORB (256*4=1KB) + RIRB (256*8=2KB), page-aligned, and bring
     * them online BEFORE probing the codec (we need verbs to do the probe). */
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, (UINTN*)&g_corb)) || !g_corb)
        return -4;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, (UINTN*)&g_rirb)) || !g_rirb)
        return -4;
    gBS->SetMem(g_corb, 4096, 0);
    gBS->SetMem(g_rirb, 4096, 0);
    /* DIAGNOSTIC: the CORB/RIRB are DMA buffers; QEMU reads them at the
     * guest PHYSICAL address we program into CORB(R)BASE. If AllocatePages
     * handed us an address >= 4GB, the 32-bit low base would truncate to 0
     * and QEMU would read garbage (every verb's cad mismatches -> no RIRB).
     * Print the full 64-bit address to rule this out. */
    con_print_ascii("hda: corb pa lo="); dbghex((uint32_t)(UINTN)g_corb);
    con_print_ascii(" hi="); dbghex((uint32_t)((UINTN)g_corb >> 32));
    con_print_ascii("\r\n");
    con_print_ascii("hda: rirb pa lo="); dbghex((uint32_t)(UINTN)g_rirb);
    con_print_ascii(" hi="); dbghex((uint32_t)((UINTN)g_rirb >> 32));
    con_print_ascii("\r\n");

    /* CRITICAL QEMU quirk: the response-queue depth gate that
     * intel_hda_corb_run checks is  if (d->rirb_count == d->rirb_cnt) return;
     * and d->rirb_cnt is driven by the RIRB INTERRUPT COUNT register
     * (RINTCNT @ 0x5A, 8-bit), NOT by CORBSIZE/RIRBSIZE (which QEMU ignores:
     * those regs have wmask=0). At reset rirb_cnt==0, so EVERY verb is
     * silently dropped and the codec can never answer. Program RINTCNT to a
     * non-zero depth (use the max, 0xFF) so verbs actually get processed. */
    hbwb(0x5A, 0xFF);                            /* RINTCNT = 255 (response queue depth) */

    /* CORB */
    hbw2(0x4A, 0x8000);                         /* CORBRP: reset read ptr */
    hbw2(0x4A, 0);                              /* CORBRP: clear reset */
    dcache_clean(g_corb, 256*4);
    hww(0x40, (uint32_t)(UINTN)g_corb);        /* CORBLBASE (low 32-bit) */
    hww(0x44, (uint32_t)((UINTN)g_corb >> 32)); /* CORBUBASE (high 32-bit) */
    hbwb(0x4C, 0x02);                           /* CORBCTL: RUN (bit1=DMA enable) */
    g_corb_wp = 1;                             /* QEMU reads (corb_rp+1) first, so entry 0 is unused */

    /* RIRB */
    hbw2(0x58, 0x8000);                         /* RIRBWP: reset */
    dcache_clean(g_rirb, 256*8);
    hww(0x50, (uint32_t)(UINTN)g_rirb);        /* RIRBLBASE (low 32-bit) */
    hww(0x54, (uint32_t)((UINTN)g_rirb >> 32)); /* RIRBUBASE (high 32-bit) */
    hbwb(0x5C, 0x03);                           /* RIRBCTL: run (DMA) + response IRQ */
    g_rirb_rp = hwr(0x58) & 0xff;               /* capture RIRBWP baseline */
    gBS->Stall(1000);

    /* diagnostics: confirm the engine is actually running and buffer is live */
    con_print_ascii("hda: CORBCTL="); dbghex(hwr(0x4C));
    con_print_ascii(" RIRBCTL="); dbghex(hwr(0x5C));
    con_print_ascii(" RINTCNT="); dbghex((uint32_t)hbrb(0x5A));
    con_print_ascii(" CORBWP="); dbghex(hwr(0x48));
    con_print_ascii(" RIRBWP="); dbghex(hwr(0x58));
    con_print_ascii(" CORBRP="); dbghex(hwr(0x4A)); con_print_ascii("\r\n");
    con_print_ascii("hda: corb[0]="); dbghex(((volatile uint32_t*)g_corb)[0]);
    con_print_ascii(" corb[1]="); dbghex(((volatile uint32_t*)g_corb)[1]);
    con_print_ascii("\r\n");

    /* wake the codec: after controller reset the codec is in D3 (off).
     * Send Set Power State D0 (verb 0x705, param 0) to the AFG node(s)
     * before any other verb, otherwise QEMU's hda-codec stays silent. */
    for (int n = 0; n < 3; n++)
        (void)hda_verb(0, n, 0x705, 0x00);
    for (int n = 0; n < 3; n++)
        (void)hda_verb(1, n, 0x705, 0x00);
    con_print_ascii("hda: codec power-on sent\r\n");

    /* detailed single-verb diagnostic: send cad0/nid1 GetVendorID and dump
     * the RIRB buffer to see if QEMU delivers ANY response at all. */
    uint32_t tv = hda_verb(0, 1, 0xF00, 0x00);
    con_print_ascii("hda: testverb(cad0,nid1)="); dbghex(tv); con_print_ascii("\r\n");
    con_print_ascii("hda: RIRBWP="); dbghex(hwr(0x58));
    con_print_ascii(" CORBWP="); dbghex(hwr(0x48)); con_print_ascii("\r\n");
    con_print_ascii("hda: rirb[0..7]:\r\n");
    for (int i = 0; i < 8; i++) {
        con_print_ascii("  "); dbghex((uint32_t)(i));
        con_print_ascii(": "); dbghex(((volatile uint32_t*)g_rirb)[i*2]);
        con_print_ascii(" / "); dbghex(((volatile uint32_t*)g_rirb)[i*2+1]);
        con_print_ascii("\r\n");
    }
    con_print_ascii("hda: corb[1]="); dbghex(((volatile uint32_t*)g_corb)[1]);
    con_print_ascii("\r\n");
    /* cache-coherency check: invalidate our view of the CORB and re-read the
     * verb we wrote. If it's still 0x001F0000 the value reached RAM (clean
     * worked); if it dropped to 0, the verb never left the CPU cache and
     * QEMU's DMA read a stale 0. */
    dcache_inv(g_corb, 256*4);
    con_print_ascii("hda: corb[1] after inv="); dbghex(((volatile uint32_t*)g_corb)[1]);
    con_print_ascii("\r\n");

    /* FOCUSED IC test for the qtree-reported cad 0 / nid 1: print ICS state
     * immediately after the verb is issued (synchronous in QEMU) to learn WHY
     * it fails. ics0: 0x02=VALID(cmd done), 0x01=BUSY stuck(find failed),
     * 0x00=BUSY never latched(IC not triggered). */
    {
        uint32_t tv = HDA_VERB(0, 1, 0xF00, 0x00);
        hww(0x60, tv); hbw2(0x68, 0x01);
        uint32_t ics0 = hwr(0x68);
        int guard = 0;
        while (!(hwr(0x68) & 0x02) && guard++ < 8000) gBS->Stall(10);
        uint32_t resp = hwr(0x64);
        uint32_t ics1 = hwr(0x68);
        con_print_ascii("hda: IC focus cad0 n1: ics0="); dbghex(ics0);
        con_print_ascii(" ics1="); dbghex(ics1);
        con_print_ascii(" resp="); dbghex(resp); con_print_ascii("\r\n");
    }
    /* Wide IC scan cad 0..15 (just report any real hit). */
    for (int c = 0; c < 16; c++) {
        for (int n = 0; n < 3; n++) {
            uint32_t icv = hda_ic_verb(HDA_VERB(c, n, 0xF00, 0x00));
            if ((icv & 0xffffff00) == 0x1af40000) {
                con_print_ascii("hda: IC HIT cad"); dbghex((uint32_t)c);
                con_print_ascii(" nid"); dbghex((uint32_t)n);
                con_print_ascii(" vendor="); dbghex(icv); con_print_ascii("\r\n");
            }
        }
    }
    con_print_ascii("hda: IC scan done\r\n");

    /* probe cad 0..3 x nid 0..2. QEMU hda-output answers GetVendorID
     * (0xF00/0x00) with 0x1af4xxxx. nid 0 (root) may not be handled by
     * QEMU's codec, so we also try the AFG nodes (1,2). STATESTS is 0. */
    int cad = -1; uint32_t vid = 0;
    for (int c = 0; c < 4 && cad < 0; c++) {
        for (int n = 0; n < 3; n++) {
            uint32_t v = hda_verb(c, n, 0xF00, 0x00);
            con_print_ascii("hda: cad"); dbghex((uint32_t)c);
            con_print_ascii(" nid"); dbghex((uint32_t)n);
            con_print_ascii(" vendor="); dbghex(v); con_print_ascii("\r\n");
            if ((v & 0xffff0000) == 0x1af40000) { cad = c; vid = v; break; }
        }
    }
    if (cad < 0) {
        con_print_ascii("hda: no codec found on any cad/nid\r\n");
        return -3;
    }
    g_hda_cad = cad;
    g_hda_cad = cad;
    g_hda_vid = vid;

    /* configure codec widgets: QEMU hda-output = AFG 1, DAC 2, Pin 3.
     * Format 0x4010 = 44.1 kHz (AC_FMT_BASE_44K) | 16-bit (AC_FMT_BITS_16), mono. */
    int dac = 2, pin = 3;
    hda_verb(cad, dac, 0x200, 0x4010);          /* Set Converter Format */
    hda_verb(cad, dac, 0x706, (1<<4)|0);        /* Set Converter Stream/Channel: stream1 ch0 */
    hda_verb(cad, dac, 0x300, 0xE04A);          /* Set Amp Gain/Mute: set L+R out, unmute, gain */
    hda_verb(cad, pin, 0x707, 0x40);            /* Set Pin Widget Control: OUT_EN */

    g_hda_sd = 0x100;                           /* output SD index 4 -> base 0x100 */
    g_hda_ok = 1;
    return 0;
}

/* play a PCM buffer (caller-owned, identity-mapped). byte_len bytes,
 * 44.1k/16/mono. returns 0 on success, else -errno. */
static long hda_play(const void *ubuf, long byte_len) {
    if (hda_init() != 0) return -1;
    if (!ubuf || byte_len <= 0) return -1;
    long nbytes = (byte_len + 3) & ~3LL;
    if (nbytes > 16*1024*1024) nbytes = 16*1024*1024;   /* sanity cap 16 MB */

    long pcmp_pages = (nbytes + 4095)/4096 + 1;
    UINTN pcmp = 0, pbdl = 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pcmp_pages, &pcmp))) return -2;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &pbdl))) {
        gBS->FreePages(pcmp, pcmp_pages); return -2;
    }
    uint8_t *pcm = (uint8_t *)(UINTN)pcmp;
    uint8_t *bdl = (uint8_t *)(UINTN)pbdl;
    gBS->SetMem(pcm, pcmp_pages*4096, 0);
    gBS->SetMem(bdl, 4096, 0);

    gBS->CopyMem(pcm, (VOID *)(UINTN)ubuf, byte_len);   /* copy caller PCM -> DMA buf */

    /* build BDL: <=64KB entries, IOC on the last one */
    long remaining = nbytes;
    uint64_t off = 0; int ne = 0;
    uint64_t pcm_pa = (uint64_t)pcmp;
    while (remaining > 0 && ne < 256) {
        uint32_t chunk = (remaining > 0x10000) ? 0x10000 : (uint32_t)remaining;
        volatile uint32_t *e = (volatile uint32_t *)(bdl + ne*16);
        e[0] = (uint32_t)(pcm_pa + off);
        e[1] = (uint32_t)((pcm_pa + off) >> 32);
        e[2] = chunk;
        e[3] = (remaining - chunk <= 0) ? 1u : 0u;        /* IOC on last */
        off += chunk; remaining -= chunk; ne++;
    }
    long cbl = nbytes;
    long lvi = ne - 1;

    dcache_clean(pcm, nbytes);
    dcache_clean(bdl, ne*16);

    uint32_t sd = g_hda_sd;
    hww(sd+0x00, hwr(sd+0x00) | 0x01);          /* reset */
    gBS->Stall(100);
    hww(sd+0x00, hwr(sd+0x00) & ~0x01u);        /* clear reset */
    gBS->Stall(100);
    hww(sd+0x12, 0x4010);                        /* SDnFMT: 44.1k mono 16-bit */
    hww(sd+0x08, (uint32_t)cbl);                /* SDnCBL */
    hww(sd+0x18, (uint32_t)(UINTN)pbdl);        /* SDnBDPL @0x18 */
    hww(sd+0x1c, 0);                             /* SDnBDPU @0x1c */
    hww(sd+0x0C, (uint16_t)lvi);                /* SDnLVI */
    hww(sd+0x00, (1u<<20) | 0x02);             /* run, stream tag 1 (bits 23:20) */

    int ok = 0;
    for (int t = 0; t < 60000; t++) {            /* up to ~60 s */
        if (hwr(sd+0x00) & (1u<<26)) { ok = 1; break; }
        gBS->Stall(1000);
    }
    hww(sd+0x00, hwr(sd+0x00) & ~0x02u);        /* stop */
    gBS->Stall(100);
    hww(sd+0x00, hwr(sd+0x00) | 0x01);          /* reset */
    gBS->Stall(50);
    hww(sd+0x00, hwr(sd+0x00) & ~0x01u);

    gBS->FreePages(pcmp, pcmp_pages);
    gBS->FreePages(pbdl, 1);
    return ok ? 0 : -3;
}

/* shell command: probe the card and play a short 440 Hz triangle tone.
 * Used both interactively and by the AUTOTEST boot path. */
static void dbghex(uint32_t v) {
    static char h[16]; int i = 0;
    for (int s = 28; s >= 0; s -= 4) {
        int d = (v >> s) & 0xf; h[i++] = (d < 10) ? (char)('0'+d) : (char)('A'-10+d);
    }
    h[8] = 0; con_print_ascii(h);
}
/* dump controller + CORB/RIRB state for headless debugging */
static void hda_dump(void) {
    con_print_ascii("hda: GCTL=");   dbghex(hwr(0x08));
    con_print_ascii(" ST=");          dbghex(hwr(0x0E));
    con_print_ascii(" CORBCTL=");     dbghex(hwr(0x4C));
    con_print_ascii(" RIRBCTL=");     dbghex(hwr(0x5C));     con_print_ascii("\r\n");
    con_print_ascii("hda: CORBWP=");  dbghex(hwr(0x48));
    con_print_ascii(" RIRBWP=");      dbghex(hwr(0x58));
    con_print_ascii(" CORBL=");        dbghex(hwr(0x40));
    con_print_ascii(" RIRBL=");        dbghex(hwr(0x50));     con_print_ascii("\r\n");
    con_print_ascii("hda: corb[0]="); dbghex(((volatile uint32_t*)g_corb)[0]);
    con_print_ascii(" corb[1]=");     dbghex(((volatile uint32_t*)g_corb)[1]);
    con_print_ascii(" rirb[0]=");      dbghex(((volatile uint32_t*)g_rirb)[0]);
    con_print_ascii(" rirb[1]=");      dbghex(((volatile uint32_t*)g_rirb)[1]); con_print_ascii("\r\n");
}
static void cmd_sndtest(void) {
    int rc = hda_init();
    if (rc != 0) {
        con_print_ascii("sndtest: audio init failed rc="); dbghex((uint32_t)(int)rc);
        con_print_ascii(" (no HDA device?)\r\n");
        return;
    }
    con_print_ascii("sndtest: cad=");
    { static char d[8]; int i=0,t=g_hda_cad; if(t==0)d[i++]='0'; while(t){d[i++]='0'+(t%10);t/=10;} for(int j=0;j<i/2;j++){char c=d[j];d[j]=d[i-1-j];d[i-1-j]=c;} d[i]=0; con_print_ascii(d); }
    con_print_ascii("\r\n");
    hda_dump();
    /* dump raw codec responses to locate the problem */
    uint32_t v0 = hda_verb(g_hda_cad, 0, 0xF00, 0x00);   /* root vendor */
    con_print_ascii("sndtest: after verb0 RIRBWP="); dbghex(hwr(0x58));
    con_print_ascii(" rirb[1]="); dbghex(((volatile uint32_t*)g_rirb)[1*2]);
    con_print_ascii(" rirb[2]="); dbghex(((volatile uint32_t*)g_rirb)[2*2]);
    con_print_ascii("\r\n");
    uint32_t v1 = hda_verb(g_hda_cad, 1, 0xF00, 0x00);   /* AFG vendor */
    uint32_t n0 = hda_verb(g_hda_cad, 0, 0xF00, 0x04);   /* root node count */
    uint32_t n1 = hda_verb(g_hda_cad, 1, 0xF00, 0x04);   /* AFG node count */
    con_print_ascii("sndtest: vid0=0x"); dbghex(v0);
    con_print_ascii(" vid1=0x"); dbghex(v1);
    con_print_ascii(" nc0=0x"); dbghex(n0);
    con_print_ascii(" nc1=0x"); dbghex(n1); con_print_ascii("\r\n");

    int sr = 44100, nsamp = (int)(sr * 0.4);
    static int16_t tpcm[44100];
    int per = sr / 440; if (per < 1) per = 1;
    for (int i = 0; i < nsamp; i++) {
        double ph = (double)(i % per) / (double)per;       /* 0..1 */
        double tri = (ph < 0.5) ? (ph*4.0 - 1.0) : (3.0 - ph*4.0);
        tpcm[i] = (int16_t)(tri * 9000.0);
    }
    con_print_ascii("sndtest: playing 440Hz tone (~0.4s)...\r\n");
    long r = hda_play(tpcm, (long)nsamp * 2);
    if (r == 0) con_print_ascii("sndtest: playback complete (IOC)\r\n");
    else        con_print_ascii("sndtest: playback failed\r\n");
}

/* The dispatcher is referenced by address and called by userland via x3. */
static long sys_dispatch(long n, long a, long b, long c, long d, long e) {
    switch (n) {
    case 64: { /* write(fd, buf, count) */
        int fd = (int)a; const char *buf = (const char *)(UINTN)b; long count = c;
        if (count < 0) return -1;
        EL0CHK(buf, (unsigned long long)count);
        if ((fd == 1 || fd == 2) && g_exec_out) {     /* terminal capture mode */
            int room = g_exec_out_cap - g_exec_out_len;
            int n = (int)count; if (n > room) n = room;
            for (int i = 0; i < n; i++) g_exec_out[g_exec_out_len + i] = buf[i];
            g_exec_out_len += n;
            return count;
        }
        if (fd == 1 || fd == 2) { con_write_bytes(buf, (int)count); return count; }
        if (fd >= 3 && g_fds[fd]) {
            UINTN ws = (UINTN)count;
            EFI_STATUS st = g_fds[fd]->Write(g_fds[fd], &ws, (VOID *)(UINTN)buf);
            if (EFI_ERROR(st)) return -1;
            return (long)ws;
        }
        return -1;
    }
    case 63: { /* read(fd, buf, count) */
        int fd = (int)a; char *buf = (char *)(UINTN)b; long count = c;
        if (count < 0) return -1;
        EL0CHK(buf, (unsigned long long)count);
        if (fd == 0 && g_exec_out) {                  /* terminal capture: feed child stdin */
            for (;;) {
                if (g_exec_in_rd < g_exec_in_len) {   /* input available -> hand it over */
                    int avail = g_exec_in_len - g_exec_in_rd;
                    int n = (int)count; if (n > avail) n = avail;
                    for (int i = 0; i < n; i++) buf[i] = g_exec_in[g_exec_in_rd + i];
                    g_exec_in_rd += n;
                    return n;
                }
                if (g_co_state != 1 && g_co_state != 2) return 0;  /* no live child -> EOF */
                g_co_state = 2;                        /* blocked waiting for input */
                el0_co_yield(0); /* yield to GUI; resumed here (kernel-resume) */
            }
        }
        if (fd == 0) return con_read_bytes(buf, (int)count);
        if (fd >= 3 && g_fds[fd]) {
            UINTN rs = (UINTN)count;
            EFI_STATUS st = g_fds[fd]->Read(g_fds[fd], &rs, buf);
            if (EFI_ERROR(st)) return -1;
            return (long)rs;
        }
        return -1;
    }
    case 57: { /* close(fd) */
        int fd = (int)a;
        if (fd >= 3 && g_fds[fd]) {
            g_fds[fd]->Flush(g_fds[fd]);          /* make sure writes hit the disk */
            g_fds[fd]->Close(g_fds[fd]);
            g_fds[fd] = NULL; return 0;
        }
        return -1;
    }
    case 56: { /* openat(dirfd, path, flags, mode) -- dirfd ignored, relative to cwd */
        const char *path = (const char *)(UINTN)b;
        long flags = c;
        EL0STR(path);
        UINT64 mode = EFI_FILE_MODE_READ;
        if (flags & 0x40) mode |= EFI_FILE_MODE_CREATE;   /* O_CREAT */
        if (flags & (0x01 | 0x02)) mode |= EFI_FILE_MODE_WRITE;  /* O_WRONLY | O_RDWR */
        char full[300]; join_path(gCwd, path, full);
        EFI_FILE_PROTOCOL *f = NULL;
        /* O_TRUNC: if the file already exists, delete it so the new write
         * starts from an empty file (EFI has no real truncate). */
        if (flags & 0x200) {
            EFI_FILE_PROTOCOL *ex = NULL;
            if (fs_open_by_path(full, EFI_FILE_MODE_READ, 0, &ex) == 0 && ex) {
                ex->Delete(ex);   /* deletes and closes the handle */
                ex = NULL;
            }
        }
        EFI_STATUS st = (fs_open_by_path(full, mode, 0, &f) != 0) ? EFI_NOT_FOUND : 0;
        if (EFI_ERROR(st) || !f) return -1;
        int fd = alloc_fd();
        if (fd < 0) { f->Close(f); return -1; }
        g_fds[fd] = f;
        return fd;
    }
    case 62: { /* lseek(fd, offset, whence) */
        int fd = (int)a; long off = b; long whence = c;
        if (fd >= 3 && g_fds[fd]) {
            UINT64 cur = 0; g_fds[fd]->GetPosition(g_fds[fd], &cur);
            UINT64 pos = cur;
            if (whence == 0) pos = (UINT64)off;
            else if (whence == 1) pos = cur + (UINT64)off;
            else { /* SEEK_END: need file size */
                EFI_GUID infoId = EFI_FILE_INFO_ID;
                UINT8 ib[128]; UINTN isz = sizeof(ib);
                if (!EFI_ERROR(g_fds[fd]->GetInfo(g_fds[fd], &infoId, &isz, ib)))
                    pos = ((EFI_FILE_INFO *)ib)->FileSize + (UINT64)off;
            }
            if (EFI_ERROR(g_fds[fd]->SetPosition(g_fds[fd], pos))) return -1;
            return (long)pos;
        }
        return -1;
    }
    case 93:  /* exit */
    case 94:  /* exit_group */
        if (g_co_state == 1 || g_co_state == 2) {     /* coroutine child called exit */
            co_child_cleanup();
            g_co_state = 3;
            el0_co_yield(0);     /* does not return */
        }
        g_elf_exited = 1; g_elf_rc = a; return a;
    case 172: return 1;                 /* getpid */
    case 174:
    case 175: return 0;                 /* getuid / geteuid */
    case 214: return 0x60000000;        /* brk (no-op) */
    /* ---- hudos-server graphics / input extensions ---- */
    case 400: { /* gop_info: a = u64[5] {base,w,h,stride,fmt}; ret 0/-1 */
        UINT64 *p = (UINT64 *)(UINTN)a;
        if (!gGop) return -1;
        EL0CHK(p, 40);
        /* Handing the shadow buffer to an unprivileged program only works if we
         * also give it EL0 access to that range (games.elf writes it directly);
         * it is revoked when the program exits. */
        if (g_el0_active && gFbBltOnly && gFbShadow && gFbShadowSize)
            el0_grant_pages((unsigned long long)(unsigned long)gFbShadow,
                            (unsigned long long)gFbShadowSize);
        /* On Blt-only devices gFbBase is 0; hand back the shadow buffer address
         * (userland only reads w/h/stride from this call, never writes the ptr). */
        p[0] = gFbBltOnly ? (UINT64)(UINTN)gFbShadow : gFbBase;
        p[1]=gFbW; p[2]=gFbH; p[3]=gFbStride; p[4]=gFbFormat;
        return 0;
    }
    case 401: { /* gop_fill(x,y,w,h,rgb); ret 0/-1 */
        if (!gGop || (!gFbBase && !gFbBltOnly)) return -1;
        fb_fill((UINT32)a,(UINT32)b,(UINT32)c,(UINT32)d,(UINT32)e);
        return 0;
    }
    case 402: { /* gop_pix(x,y,rgb); ret 0/-1 */
        if (!gGop || (!gFbBase && !gFbBltOnly)) return -1;
        fb_set_px((UINT32)a,(UINT32)b,(UINT32)c);
        return 0;
    }
    case 403: { /* mouse_get(a = int[4] {x,y,buttons,clicked}) */
        EL0CHK((void *)(UINTN)a, 16);
        int *p = (int *)(UINTN)a;
        long r = mouse_poll(p);
        gop_present();                    /* flush shadow -> screen for Blt-only GOP */
        return r;
    }
    case 404: { /* kbd_poll non-blocking: return unicode char or 0 */
        long r = 0;
        if (gConIn) {
            EFI_INPUT_KEY k;
            EFI_STATUS st = gConIn->ReadKeyStroke(gConIn, &k);
            if (!EFI_ERROR(st)) r = (long)k.UnicodeChar;
        }
        gop_present();
        return r;
    }
    case 405: { /* sleep(ms) */
        gBS->Stall((UINTN)a * 1000);
        gop_present();
        if (g_co_yield_on_input && g_co_state == 1) {
            g_co_state = 2;
            el0_co_yield(1);   /* per-frame yield (completed syscall -> eret frame) */
        }
        return 0;
    }
    case 406: { /* kbd_get(a = int[2]): arr[0]=unicode, arr[1]=scancode; ret 1 if key present else 0 */
        EL0CHK((void *)(UINTN)a, 8);
        int *p = (int *)(UINTN)a;
        long r = 0;
        if (gConIn) {
            EFI_INPUT_KEY k;
            EFI_STATUS st = gConIn->ReadKeyStroke(gConIn, &k);
            if (!EFI_ERROR(st)) { p[0]=(int)k.UnicodeChar; p[1]=(int)k.ScanCode; r = 1; }
            else { p[0]=0; p[1]=0; }
        } else { p[0]=0; p[1]=0; }
        gop_present();
        return r;
    }
    case 441: { /* kbd_get_ex(a = int[3]): {unicode, scancode, shiftstate}; ret 1 if key else 0.
                     Uses ReadKeyStrokeEx when available so the modifier state is captured for
                     THIS key event (reliable for Command/Ctrl/Alt combos); falls back to the
                     basic ReadKeyStroke (shiftstate 0) otherwise. */
        int *p = (int *)(UINTN)a;
        long r = 0;
        if (gConInEx) {
            EFI_KEY_DATA kd;
            EFI_STATUS st = gConInEx->ReadKeyStrokeEx(gConInEx, &kd);
            if (!EFI_ERROR(st)) {
                p[0] = (int)kd.Key.UnicodeChar;
                p[1] = (int)kd.Key.ScanCode;
                p[2] = (int)kd.KeyState.KeyShiftState;
                r = 1;
            } else { p[0]=0; p[1]=0; p[2]=0; }
        } else if (gConIn) {
            EFI_INPUT_KEY k;
            EFI_STATUS st = gConIn->ReadKeyStroke(gConIn, &k);
            if (!EFI_ERROR(st)) { p[0]=(int)k.UnicodeChar; p[1]=(int)k.ScanCode; p[2]=0; r=1; }
            else { p[0]=0; p[1]=0; p[2]=0; }
        } else { p[0]=0; p[1]=0; p[2]=0; }
        gop_present();
        return r;
    }
    case 442: { /* gop_present(): flush shadow -> screen for Blt-only GOP */
        gFbDirty = 1;
        gop_present();
        /* If this is a gui-launched coroutine child, yield back to the gui parent
         * after we've drawn+presented our frame, so gui can draw its chrome and
         * detect a quit. The yield happens exactly once per frame (apps call
         * gop_present once per frame), giving clean flicker-free co-hosting. */
        if (g_co_yield_on_input && g_co_state == 1) {
            g_co_state = 2;
            el0_co_yield(1);   /* yields; resumed next co_resume via eret frame */
        }
        return 0;
    }
    case 419: { /* kbd_mods(): return current modifier shift-state (0 if Ex proto unavailable) */
        UINT32 st = 0;
        if (gConInEx) {
            EFI_KEY_STATE ks;
            if (!EFI_ERROR(gConInEx->GetState(gConInEx, &ks))) st = (UINT32)ks.KeyShiftState;
        }
        return (long)st;
    }
    /* ---- hudos-server TUI (text-console) extensions ---- */
    case 407: { /* con_size(a = int[2] {cols,rows}) */
        int *p = (int *)(UINTN)a;
        if (!gST || !gST->ConOut) { p[0]=80; p[1]=25; return 0; }
        EFI_SIMPLE_TEXT_OUTPUT_MODE *mode =
            (EFI_SIMPLE_TEXT_OUTPUT_MODE *)gST->ConOut->Mode;
        EFI_TEXT_QUERY_MODE qm =
            (EFI_TEXT_QUERY_MODE)gST->ConOut->QueryMode;
        UINTN cols=80, rows=25;
        if (mode && qm && !EFI_ERROR(qm(gST->ConOut, (UINTN)mode->Mode, &cols, &rows))) {
            p[0]=(int)cols; p[1]=(int)rows;
        } else { p[0]=80; p[1]=25; }
        return 0;
    }
    case 408: { /* con_clear() */
        if (gST && gST->ConOut && gST->ConOut->ClearScreen)
            gST->ConOut->ClearScreen(gST->ConOut);
        return 0;
    }
    case 409: { /* con_put(col,row,ch,attr): draw one char with text attribute */
        if (!gST || !gST->ConOut) return -1;
        int col=(int)a, row=(int)b;
        CHAR16 w[2]; w[0]=(CHAR16)((unsigned)c & 0xFFFF); w[1]=0;
        EFI_TEXT_SET_ATTRIBUTE sa = (EFI_TEXT_SET_ATTRIBUTE)gST->ConOut->SetAttribute;
        EFI_TEXT_SET_CURSOR_POSITION sp =
            (EFI_TEXT_SET_CURSOR_POSITION)gST->ConOut->SetCursorPosition;
        sa(gST->ConOut, (UINTN)d & 0x7F);
        sp(gST->ConOut, (UINTN)col, (UINTN)row);
        gST->ConOut->OutputString(gST->ConOut, w);
        return 0;
    }
    case 410: { /* con_cursor(visible): toggle hardware text cursor */
        if (gST && gST->ConOut && gST->ConOut->EnableCursor) {
            EFI_TEXT_ENABLE_CURSOR ec =
                (EFI_TEXT_ENABLE_CURSOR)gST->ConOut->EnableCursor;
            ec(gST->ConOut, a ? 1 : 0);
        }
        return 0;
    }
    /* ---- hudos-server desktop integration (tdesktop "Applications") ---- */
    case 411: { /* exec_app(path, outbuf, outcap): load + run an ELF with its
                   stdout/stderr captured into outbuf (caller's memory). Returns
                   the number of captured bytes, or -1 on load/open failure. */
        const char *path = (const char *)(UINTN)a;
        char *out = (char *)(UINTN)b; int outcap = (int)c;
        if (!path || !out || outcap <= 0) return -1;
        EFI_FILE_PROTOCOL *f = NULL;
        if (open_rel(path, EFI_FILE_MODE_READ, 0, &f) != 0) return -1;
        EFI_GUID infoId = EFI_FILE_INFO_ID;
        UINT8 ib[128]; UINTN isz = sizeof(ib); UINT64 fsize = 0;
        if (!EFI_ERROR(f->GetInfo(f, &infoId, &isz, ib))) fsize = ((EFI_FILE_INFO *)ib)->FileSize;
        if (fsize == 0) { f->Close(f); return -1; }
        VOID *img = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 16, &img))) {
            f->Close(f); return -1;
        }
        UINTN rs = (UINTN)fsize;
        if (EFI_ERROR(f->Read(f, &rs, img))) {
            gBS->FreePool(img); f->Close(f); return -1;
        }
        f->Close(f);
        UINT64 entry, base, pages, sp;
        if (elf_load((const uint8_t *)img, (UINT64)fsize, &entry, &base, &pages, &sp) != 0) {
            gBS->FreePool(img); return -1;
        }
        /* argv[0] = path (the child may print it; no extra args) */
        VOID *pool = NULL; char **argv = NULL; int argc = 0;
        if (!EFI_ERROR(gBS->AllocatePool(EfiLoaderData, 512, &pool)) && pool) {
            argv = (char **)pool;
            argv[0] = (char *)pool + 128;
            int pi = 0; for (int i = 0; path[i] && pi < 127; i++) argv[0][pi++] = path[i];
            argv[0][pi] = 0; argc = 1;
        }
        for (int i = 0; i < MAXFD; i++) g_fds[i] = NULL;
        g_elf_exited = 0; g_elf_rc = 0;
        g_exec_out = out; g_exec_out_cap = outcap; g_exec_out_len = 0;
        {
            int go_el0 = g_el0_enable;
            if (go_el0) {
                /* Let the unprivileged program touch its own image + stack and
                 * the page holding argv. If a walk fails we stay at EL1. */
                if (el0_grant_pages(base, (unsigned long long)pages * 4096ULL) != 0) go_el0 = 0;
                else if (pool && el0_grant_pages((unsigned long long)(unsigned long)pool & ~0xFFFULL,
                                                 0x1000) != 0) go_el0 = 0;
            }
            if (go_el0) el0_run(entry, sp, (UINT64)argc, (UINT64)argv, 0);
            else        call_elf(entry, (UINT64)argc, (UINT64)argv, 0, (UINT64)sys_dispatch, sp);
            if (g_el0_gn) el0_ungrant_pages();
        }
        g_exec_out = NULL;
        for (int i = 0; i < MAXFD; i++) if (g_fds[i]) { g_fds[i]->Close(g_fds[i]); g_fds[i] = NULL; }
        gBS->FreePool(pool); gBS->FreePages(base, pages); gBS->FreePool(img);
        return g_exec_out_len;       /* >=0 captured bytes */
    }
    case 425: { /* exec_argv(cmdline, outbuf, outcap): like 411 but parse a
                   space-separated command line into argv so the child receives
                   arguments (e.g. "app/imgview.elf picture/test1.png"). The
                   blocking call returns the number of captured bytes, or -1. */
        const char *rest = (const char *)(UINTN)a;
        if (!rest || !*rest) return -1;
        el0_puts("CO_ARGV_LAUNCH: "); el0_puts(rest); el0_puts("\n");
        char *out = (char *)(UINTN)b; int outcap = (int)c;
        if (!out || outcap <= 0) return -1;
        /* extract the program filename (first token) to open/load */
        const char *fp = rest;
        while (*fp == ' ' || *fp == '\t') fp++;
        const char *ep = fp;
        while (*ep && *ep != ' ' && *ep != '\t') ep++;
        int fnlen = (int)(ep - fp);
        if (fnlen == 0 || fnlen > 255) return -1;
        char fname[256];
        for (int i = 0; i < fnlen; i++) fname[i] = fp[i];
        fname[fnlen] = 0;
        /* load the ELF the same way as 411 */
        EFI_FILE_PROTOCOL *f = NULL;
        if (open_rel(fname, EFI_FILE_MODE_READ, 0, &f) != 0) return -1;
        EFI_GUID infoId = EFI_FILE_INFO_ID;
        UINT8 ib[128]; UINTN isz = sizeof(ib); UINT64 fsize = 0;
        if (!EFI_ERROR(f->GetInfo(f, &infoId, &isz, ib))) fsize = ((EFI_FILE_INFO *)ib)->FileSize;
        if (fsize == 0) { f->Close(f); return -1; }
        VOID *img = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 16, &img))) {
            f->Close(f); return -1;
        }
        UINTN rs = (UINTN)fsize;
        if (EFI_ERROR(f->Read(f, &rs, img))) {
            gBS->FreePool(img); f->Close(f); return -1;
        }
        f->Close(f);
        UINT64 entry, base, pages, sp;
        if (elf_load((const uint8_t *)img, (UINT64)fsize, &entry, &base, &pages, &sp) != 0) {
            gBS->FreePool(img); return -1;
        }
        /* argv: parse the WHOLE command line so argv[0]=program, argv[1]=arg... */
        char **argvp; VOID *pool;
        int argc = build_argv((char *)rest, &argvp, &pool);
        for (int i = 0; i < MAXFD; i++) g_fds[i] = NULL;
        g_elf_exited = 0; g_elf_rc = 0;
        g_exec_out = out; g_exec_out_cap = outcap; g_exec_out_len = 0;
        {
            int go_el0 = g_el0_enable;
            if (go_el0) {
                if (el0_grant_pages(base, (unsigned long long)pages * 4096ULL) != 0) go_el0 = 0;
                else if (pool && el0_grant_pages((unsigned long long)(unsigned long)pool & ~0xFFFULL,
                                                 0x1000) != 0) go_el0 = 0;
            }
            if (go_el0) el0_run(entry, sp, (UINT64)argc, (UINT64)argvp, 0);
            else        call_elf(entry, (UINT64)argc, (UINT64)argvp, 0, (UINT64)sys_dispatch, sp);
            if (g_el0_gn) el0_ungrant_pages();
        }
        g_exec_out = NULL;
        for (int i = 0; i < MAXFD; i++) if (g_fds[i]) { g_fds[i]->Close(g_fds[i]); g_fds[i] = NULL; }
        gBS->FreePool(pool); gBS->FreePages(base, pages); gBS->FreePool(img);
        return g_exec_out_len;       /* >=0 captured bytes */
    }
    case 412: { /* list_dir(path, outbuf, outcap): enumerate a directory, writing
                   for each entry a 1-byte type ('D'=directory, 'F'=file) followed by
                   the UTF-8 name and a NUL into outbuf. Returns the number of entries
                   (or -1 on error / when outbuf too small to hold the next entry). */
        const char *path = (const char *)(UINTN)a;
        char *out = (char *)(UINTN)b; int outcap = (int)c;
        if (!path || !out || outcap <= 2) return -1;
        EL0CHK(out, (unsigned long long)outcap);
        EL0STR(path);
        char full[300]; join_path(gCwd, path, full);
        EFI_FILE_PROTOCOL *d = NULL;
        if (fs_open_by_path(full, EFI_FILE_MODE_READ, 0, &d) != 0) return -1;
        UINT8 rbuf[2048]; int count = 0; int pos = 0;
        for (;;) {
            UINTN rs = sizeof(rbuf);
            EFI_STATUS st = d->Read(d, &rs, rbuf);
            if (EFI_ERROR(st) || rs == 0) break;
            UINTN off = 0;
            while (off + sizeof(EFI_FILE_INFO) <= rs) {
                EFI_FILE_INFO *fi = (EFI_FILE_INFO *)(rbuf + off);
                if (fi->Size == 0) break;
                CHAR16 *nm = fi->FileName;
                if (nm[0] == '.' && nm[1] == 0) { off += (UINTN)fi->Size; continue; }
                if (nm[0] == '.' && nm[1] == '.' && nm[2] == 0) { off += (UINTN)fi->Size; continue; }
                int isdir = (fi->Attribute & EFI_FILE_DIRECTORY_ATTR) ? 1 : 0;
                char name[256]; int li = 0;
                for (int k = 0; nm[k] && li < 255; k++) name[li++] = (char)nm[k];
                name[li] = 0;
                int need = 1 + li + 1;   /* type byte + name + NUL */
                if (pos + need > outcap) break;
                out[pos++] = isdir ? 'D' : 'F';
                for (int k = 0; k < li; k++) out[pos++] = name[k];
                out[pos++] = 0; count++;
                off += (UINTN)fi->Size;
            }
        }
        d->Close(d);
        return count;
    }
    case 424: { /* unlink(path): delete a file or (empty) directory.
                   Mirrors the shell `del` command: open with read+write, then
                   EFI Delete() (which closes + removes). Returns 0 on success,
                   -1 on error (e.g. directory not empty, or file in use). */
        const char *path = (const char *)(UINTN)a;
        if (!path || path[0] == 0) return -1;
        char full[300]; join_path(gCwd, path, full);
        EFI_FILE_PROTOCOL *f = NULL;
        if (fs_open_by_path(full, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &f) != 0) return -1;
        EFI_STATUS st = f->Delete(f);     /* on success also closes the handle */
        if (EFI_ERROR(st)) { f->Close(f); return -1; }
        return 0;
    }
    case 427: { /* copy(src, dst): recursive copy of a file or a directory.
                   src/dst are resolved relative to gCwd (like the shell).
                   Returns 0 on success, -1 on error. */
        const char *src = (const char *)(UINTN)a;
        const char *dst = (const char *)(UINTN)b;
        if (!src || !dst || src[0] == 0 || dst[0] == 0) return -1;
        char sfull[300], dfull[300];
        join_path(gCwd, src, sfull);
        join_path(gCwd, dst, dfull);
        EFI_FILE_PROTOCOL *sf = NULL;
        if (fs_open_by_path(sfull, EFI_FILE_MODE_READ, 0, &sf) != 0) return -1;
        EFI_GUID infoId = EFI_FILE_INFO_ID;
        UINT8 ib[128]; UINTN isz = sizeof(ib);
        int isdir = 0;
        if (!EFI_ERROR(sf->GetInfo(sf, &infoId, &isz, ib))) {
            if (((EFI_FILE_INFO *)ib)->Attribute & EFI_FILE_DIRECTORY_ATTR) isdir = 1;
        }
        int rc = 0;
        if (isdir) {
            EFI_FILE_PROTOCOL *df = NULL;
            if (fs_open_by_path(dfull, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                EFI_FILE_DIRECTORY_ATTR, &df) != 0) {
                sf->Close(sf); return -1;
            }
            copy_dir_into(sf, df);
            df->Close(df);
        } else {
            EFI_FILE_PROTOCOL *df = NULL;
            if (fs_open_by_path(dfull, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                0, &df) != 0) {
                sf->Close(sf); return -1;
            }
            UINT8 buf[2048];
            for (;;) {
                UINTN rs = sizeof(buf);
                EFI_STATUS st = sf->Read(sf, &rs, buf);
                if (EFI_ERROR(st) || rs == 0) break;
                UINTN ws = rs;
                st = df->Write(df, &ws, buf);
                if (EFI_ERROR(st)) { rc = -1; break; }
            }
            df->Flush(df); df->Close(df);
        }
        sf->Close(sf);
        return rc;
    }
    case 430: { /* mount(volspec, dir): mount a volume (by index or label) at an
                   absolute directory. Returns 0 on success, <0 on error. */
        const char *vol = (const char *)(UINTN)a;
        const char *dir = (const char *)(UINTN)b;
        return (long)mount_do(vol, dir);
    }
    case 431: { /* umount(dir): unmount a previously mounted directory. Returns 0 / <0. */
        const char *dir = (const char *)(UINTN)a;
        return (long)umount_do(dir);
    }
    case 432: { /* mount_list(buf, cap): fill buf with the volume + mount tables as
                   text. Returns the number of bytes written, or -1 on error. */
        char *buf = (char *)(UINTN)a; int cap = (int)b;
        if (!buf || cap <= 0) return -1;
        mount_list_text(buf, cap);
        return (long)my_strlen(buf);
    }
    case 413: { /* exec_co(path, outbuf, outcap): start an ELF as a coroutine that
                   runs *inside* the Terminal (interactive). Returns 0 / -1. The GUI
                   drives it via SYS_co_resume and feeds input via SYS_co_push. */
        const char *path = (const char *)(UINTN)a;
        char *out = (char *)(UINTN)b; int outcap = (int)c;
        if (!path || !out || outcap <= 0) return -1;
        if (g_co_state == 1 || g_co_state == 2) co_child_cleanup();  /* kill any prior child */
        EFI_FILE_PROTOCOL *f = NULL;
        if (open_rel(path, EFI_FILE_MODE_READ, 0, &f) != 0) return -1;
        EFI_GUID infoId = EFI_FILE_INFO_ID;
        UINT8 ib[128]; UINTN isz = sizeof(ib); UINT64 fsize = 0;
        if (!EFI_ERROR(f->GetInfo(f, &infoId, &isz, ib))) fsize = ((EFI_FILE_INFO *)ib)->FileSize;
        if (fsize == 0) { f->Close(f); return -1; }
        VOID *img = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 16, &img))) { f->Close(f); return -1; }
        UINTN rs = (UINTN)fsize;
        if (EFI_ERROR(f->Read(f, &rs, img))) { gBS->FreePool(img); f->Close(f); return -1; }
        f->Close(f);
        UINT64 entry, base, pages, sp;
        if (elf_load((const uint8_t *)img, (UINT64)fsize, &entry, &base, &pages, &sp) != 0) {
            gBS->FreePool(img); return -1;
        }
        g_co_child_img = img; g_co_child_base = base; g_co_child_pages = pages;
        g_child_base = base; g_child_end = base + pages * 4096ULL;
        g_co_child_stack = sp; g_co_child_entry = entry;
        g_co_child_argc = 0; g_co_child_argv = NULL;   /* no argv for plain exec_co */
        g_co_argv_pool = NULL;
        g_el0_co_first = (el0_co_setup(base, pages, NULL) == 0);
        g_el0_co = g_el0_co_first;
        for (int i = 0; i < 12; i++) g_co_child.x[i] = 0;   /* callee-saved = 0 */
        g_co_child.x[11] = (UINT64)child_entry_trampoline;  /* lr -> entry shim */
        g_co_child.sp = sp;
        for (int i = 0; i < MAXFD; i++) g_fds[i] = NULL;
        g_elf_exited = 0; g_elf_rc = 0;
        g_exec_out = out; g_exec_out_cap = outcap; g_exec_out_len = 0;
        g_exec_in_len = 0; g_exec_in_rd = 0;
        g_co_state = 1;               /* started; first SYS_co_resume runs it */
        return 0;
    }
    case 426: { /* exec_co_argv(cmdline, outbuf, outcap): like 413 but parse a
                   space-separated command line into argv (e.g.
                   "app/imgview.elf picture/x.png") so the child receives
                   arguments. Returns 0 / -1. Drive via SYS_co_resume until the
                   child exits (it then returns cleanly to the desktop). */
        const char *rest = (const char *)(UINTN)a;
        if (!rest || !*rest) return -1;
        el0_puts("CO_ARGV_LAUNCH: "); el0_puts(rest); el0_puts("\n");
        char *out = (char *)(UINTN)b; int outcap = (int)c;
        if (!out || outcap <= 0) return -1;
        if (g_co_state == 1 || g_co_state == 2) co_child_cleanup();  /* kill any prior child */
        /* extract the program filename (first token) to open/load */
        const char *fp = rest;
        while (*fp == ' ' || *fp == '\t') fp++;
        const char *ep = fp;
        while (*ep && *ep != ' ' && *ep != '\t') ep++;
        int fnlen = (int)(ep - fp);
        if (fnlen == 0 || fnlen > 255) return -1;
        char fname[256];
        for (int i = 0; i < fnlen; i++) fname[i] = fp[i];
        fname[fnlen] = 0;
        EFI_FILE_PROTOCOL *f = NULL;
        if (open_rel(fname, EFI_FILE_MODE_READ, 0, &f) != 0) return -1;
        EFI_GUID infoId = EFI_FILE_INFO_ID;
        UINT8 ib[128]; UINTN isz = sizeof(ib); UINT64 fsize = 0;
        if (!EFI_ERROR(f->GetInfo(f, &infoId, &isz, ib))) fsize = ((EFI_FILE_INFO *)ib)->FileSize;
        if (fsize == 0) { f->Close(f); return -1; }
        VOID *img = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 16, &img))) { f->Close(f); return -1; }
        UINTN rs = (UINTN)fsize;
        if (EFI_ERROR(f->Read(f, &rs, img))) { gBS->FreePool(img); f->Close(f); return -1; }
        f->Close(f);
        UINT64 entry, base, pages, sp;
        if (elf_load((const uint8_t *)img, (UINT64)fsize, &entry, &base, &pages, &sp) != 0) {
            gBS->FreePool(img); return -1;
        }
        g_co_child_img = img; g_co_child_base = base; g_co_child_pages = pages;
        g_child_base = base; g_child_end = base + pages * 4096ULL;
        g_imgf_base = (unsigned long long)(UINTN)img; g_imgf_end = g_imgf_base + fsize + 16;
        g_co_child_stack = sp; g_co_child_entry = entry;
        /* argv: parse the WHOLE command line (argv[0]=program, argv[1]=arg...) */
        char **argvp; VOID *pool;
        int argc = build_argv((char *)rest, &argvp, &pool);
        g_argp_base = pool ? (unsigned long long)(UINTN)pool : 0;
        g_argp_end  = pool ? g_argp_base + 4096 : 0;
        g_co_child_argc = argc; g_co_child_argv = argvp; g_co_argv_pool = pool;
        g_el0_co_first = (el0_co_setup(base, pages, pool) == 0);
        g_el0_co = g_el0_co_first;
        el0_puts("EL0_CO="); el0_puthex("", (unsigned long long)g_el0_co); el0_puts("\n");
        for (int i = 0; i < 12; i++) g_co_child.x[i] = 0;   /* callee-saved = 0 */
        g_co_child.x[11] = (UINT64)child_entry_trampoline;  /* lr -> entry shim */
        g_co_child.sp = sp;
        for (int i = 0; i < MAXFD; i++) g_fds[i] = NULL;
        g_elf_exited = 0; g_elf_rc = 0;
        g_exec_out = out; g_exec_out_cap = outcap; g_exec_out_len = 0;
        g_exec_in_len = 0; g_exec_in_rd = 0;
        g_co_state = 1;               /* started; first SYS_co_resume runs it */
        return 0;
    }
    case 414: { /* co_resume(): run the coroutine child until it blocks on input or
                   exits. Returns the new g_co_state (2 blocked / 3 done). */
        if (g_co_state == 1 || g_co_state == 2) {
            g_co_state = 1;          /* mark running so the child yields again next frame */
            if (g_el0_co) {
                __asm__ volatile("msr daifset, #0x3" ::: "memory");
                el0_set_vbar(g_el0_vbar);
                if (g_el0_co_first) {
                    g_el0_co_first = 0;
                    hudos_el0_co_start(g_co_child_entry, g_co_child_stack,
                                       g_el0_co_ksp, (UINT64)g_co_child_argc,
                                       (UINT64)g_co_child_argv, (UINT64)&g_co_parent);
                } else if (g_el0_resume_mode) {
                    /* Completed-syscall yield: restore the child's FULL EL0
                     * execution context (x0..x30 + elr + spsr + sp_el0) and eret
                     * straight back into EL0. This is the corrected path for
                     * gop_present/sleep yields: the old code only restored x0..x30
                     * and reused the current (stale) elr/spsr/sp_el0, so the eret
                     * dropped into a bogus EL1 address and executed a stray svc
                     * (the "打开文件管理器即崩" crash, SVC@EL1). We save the
                     * parent's kernel context first so the next yield's co_switch
                     * returns here cleanly.
                     *
                     * CRITICAL VBAR fix: hudos_el0_ctx_eret is ONE-WAY (it erets
                     * into EL0 and never returns to this function). When the child
                     * later yields again, its co_switch(&g_co_child,&g_co_parent)
                     * restores the parent context saved by the co_switch below and
                     * returns to the instruction immediately AFTER that co_switch.
                     * At that point g_fw_vbar is still installed (el0_co_yield set
                     * it for the parent) — re-ereting would run the child with the
                     * firmware vectors and crash with "Synchronous Exception". The
                     * g_el0_yield_pending flag tells the two arrivals apart: the
                     * FIRST arrival erets into the child; a return caused by a
                     * child yield hands control back to the GUI loop. */
                    __asm__ volatile("msr daifset, #0x3" ::: "memory");
                    el0_set_vbar(g_el0_vbar);
                    if (g_el0_use_pt) el0_set_ttbr0(g_el0_child_ttbr0);
                    co_switch(&g_co_parent, &g_co_parent);   /* save parent ctx; child-yield returns here */
                    if (g_el0_yield_pending) {              /* child yielded back -> return to GUI */
                        g_el0_yield_pending = 0;
                        el0_set_vbar(g_fw_vbar);
                        __asm__ volatile("msr daifclr, #0x3" ::: "memory");
                        /* If the child exited (state==3) rather than just yielding,
                         * free its resources now that we are safely back in the
                         * parent. Teardown is deferred from the exit path so that
                         * el0_co_yield's guard stays satisfied (g_el0_co must be 1
                         * when it runs). */
                        if (g_co_state == 3) co_child_cleanup();
                        return g_co_state;
                    }
                    if (!g_el0_quiet) {
                        el0_puts("[resume] child_ctx: ");
                        el0_puthex("elr=", g_co_child_ctx.elr);
                        el0_puthex("spsr=", g_co_child_ctx.spsr);
                        el0_puthex("sp_el0=", g_co_child_ctx.sp_el0);
                    }
                    __attribute__((aligned(0x100))) struct el0_ctx rf;
                    rf = g_co_child_ctx;
                    hudos_el0_ctx_eret(&rf);
                    for (;;) __asm__ volatile("wfi");   /* does not return */
                } else {
                    el0_restore_child_state();
                    co_switch(&g_co_parent, &g_co_child);
                }
                /* back in the parent: leave the EL0 window */
                el0_set_vbar(g_fw_vbar);
                __asm__ volatile("msr daifclr, #0x3" ::: "memory");
            } else {
                co_switch(&g_co_parent, &g_co_child);
            }
        }
        return g_co_state;
    }
    case 415: { /* co_push(buf, n): append bytes to the child's stdin queue (no echo). */
        const char *buf = (const char *)(UINTN)a; int n = (int)b;
        if (!buf || n <= 0) return 0;
        /* compact the consumed prefix if we're running low on room */
        if (g_exec_in_rd > 0 && g_exec_in_len + n > (int)sizeof(g_exec_in)) {
            int left = g_exec_in_len - g_exec_in_rd;
            for (int i = 0; i < left; i++) g_exec_in[i] = g_exec_in[g_exec_in_rd + i];
            g_exec_in_len = left; g_exec_in_rd = 0;
        }
        int room = (int)sizeof(g_exec_in) - g_exec_in_len;
        if (room <= 0) return 0;
        if (n > room) n = room;
        for (int i = 0; i < n; i++) g_exec_in[g_exec_in_len + i] = buf[i];
        g_exec_in_len += n;
        return n;
    }
    case 416: { /* co_kill(): terminate a still-running child and free its resources. */
        if (g_co_state == 1 || g_co_state == 2) { co_child_cleanup(); g_co_state = 0; }
        /* Clear any leftover yield/resume flags so a subsequently launched child
         * starts with a clean coroutine state (the previous child set these when
         * it last yielded; they are re-established on its next yield). */
        g_el0_resume_mode = 0;
        g_el0_yield_pending = 0;
        return 0;
    }
    case 417: { /* co_out_len(): bytes captured so far (for the GUI to render). */
        return g_exec_out_len;
    }
    case 418: { /* co_out_append(buf, n): append bytes to the captured-output buffer
                   only (used by the GUI for local echo / status lines). */
        const char *buf = (const char *)(UINTN)a; int n = (int)b;
        if (!g_exec_out || !buf || n <= 0) return 0;
        int room = g_exec_out_cap - g_exec_out_len;
        if (room <= 0) return 0;
        if (n > room) n = room;
        for (int i = 0; i < n; i++) g_exec_out[g_exec_out_len + i] = buf[i];
        g_exec_out_len += n;
        return n;
    }
    case 420: { /* sys_fetch(url, buf, cap): HTTP GET a URL (no TLS), return body len or -1 */
        const char *url = (const char *)(UINTN)a;
        char *buf = (char *)(UINTN)b;
        int cap = (int)c;
        if (!url || !buf || cap <= 0) return -1;
        return sys_fetch(url, buf, cap);
    }
    case 428: { /* co_set_yield(on): enable/disable per-frame yielding of a
                   gui-launched coroutine child on gop_present. Only the gui program
                   manager uses this; tdesktop never calls it. */
        g_co_yield_on_input = (int)a ? 1 : 0;
        return 0;
    }
    case 421: { /* net self-test: detect -> virtio init -> dhcp; report into buf */
        char *ob = (char *)(UINTN)a; int cap = (int)b;
        if (!ob || cap <= 0) return -1;
        return nw_net_selftest(ob, cap);
    }
    case 422: { /* audio_init(): probe + init HDA; ret 0/-errno */
        return hda_init();
    }
    case 423: { /* audio_play(buf, len): play 44.1k/16/mono PCM; ret 0/-errno */
        const void *buf = (const void *)(UINTN)a;
        long len = b;
        return hda_play(buf, len);
    }
    case 433: { /* mkdir(path): create directory (and parents) on the owning volume; ret 0/-1 */
        const char *p = (const char *)(UINTN)a;
        if (!p) return -1;
        fs_mkdir_vfs(p);
        return 0;
    }
    case 434: { /* volume_info(index, info_ptr, size): index<0 => return gVolCount;
                   else fill *info_ptr (vol_info_t) and return 0 / -1. */
        long idx = a;
        if (idx < 0) return (long)gVolCount;
        vol_info_t *vi = (vol_info_t *)(UINTN)b;
        if (!vi || idx >= gVolCount) return -1;
        for (int k = 0; k < (int)sizeof(*vi); k++) ((UINT8 *)vi)[k] = 0;
        vol_get_label((int)idx, vi->label, 16);
        EFI_GUID ig = EFI_FILE_SYSTEM_INFO_GUID;
        UINT8 ib[256]; UINTN isz = sizeof(ib);
        if (!EFI_ERROR(gVolRoot[(int)idx]->GetInfo(gVolRoot[(int)idx], &ig, &isz, ib))) {
            EFI_FILE_SYSTEM_INFO *fi = (EFI_FILE_SYSTEM_INFO *)ib;
            vi->total = fi->VolumeSize;
            vi->free  = fi->FreeSpace;
            if (vi->free > vi->total) vi->free = vi->total;   /* clamp */
            vi->used  = vi->total - vi->free;
        } else if (gVolBlk[(int)idx] && gVolBlk[(int)idx]->Media) {
            /* fallback when the FS driver won't report size: use raw block geometry */
            EFI_BLOCK_IO_MEDIA *mm = gVolBlk[(int)idx]->Media;
            UINT64 bsz = mm->BlockSize ? mm->BlockSize : 512;
            vi->total = ((UINT64)mm->LastBlock + 1) * bsz;
        }
        if (gVolBlk[(int)idx] && gVolBlk[(int)idx]->Media) vi->block = gVolBlk[(int)idx]->Media->BlockSize;
        vi->is_sys = (idx == 0) ? 1 : 0;
        vi->can_fmt = (idx != 0 && gVolBlk[(int)idx]) ? 1 : 0;
        return 0;
    }
    case 435: { /* format(index, label): format a non-system volume as FAT16. ret 0/-err */
        const char *lab = (const char *)(UINTN)b;
        return (long)vol_format_fat16((int)a, lab);
    }
    case 436: { /* disk_list(a=disk_info_t*, b=max): fill array, return disk count */
        k_disk_info *db = (k_disk_info *)(UINTN)a;
        int cap = (int)b;
        int n = (gDiskCount < cap) ? gDiskCount : cap;
        for (int i = 0; i < n; i++) {
            db[i].total = (UINT64)gDiskTotal[i] * gDiskBlock[i];
            db[i].block = gDiskBlock[i];
            db[i].is_sys = (i == 0) ? 1 : 0;
        }
        return gDiskCount;
    }
    case 437: { /* disk_parts(a=disk, b=part_info_t*, c=cap): return partition count */
        int disk = (int)a;
        k_part_info *pb = (k_part_info *)(UINTN)b;
        int cap = (int)c;
        if (disk < 0 || disk >= gDiskCount || !gDiskBlk[disk]) return -1;
        UINT8 mbr[512]; if (disk_read_mbr(disk, mbr)) return -2;
        int n = 0;
        for (int p = 0; p < 4; p++) {
            if (n >= cap) break;
            UINT8 *pe = mbr + MBR_PART_OFF + p*16;
            UINT32 ptype = pe[4];
            if (ptype == 0) continue;
            pb[n].start  = rd32le(pe + 8);
            pb[n].size   = rd32le(pe + 12);
            pb[n].type   = ptype;
            pb[n].active = (pe[0] == 0x80) ? 1 : 0;
            pb[n].used   = 1;
            n++;
        }
        return n;
    }
    case 438: { /* disk_part_create(a=disk, b=size_sectors, c=type): return slot / -err */
        return (long)disk_create_part((int)a, (UINT64)b, (UINT32)c);
    }
    case 439: { /* disk_part_delete(a=disk, b=part_idx): return 0 / -err */
        return (long)disk_delete_part((int)a, (int)b);
    }
    case 440: { /* disk_part_format(a=disk, b=part_idx, c=label): return 0 / -err */
        return (long)disk_format_part((int)a, (int)b, (const char *)(UINTN)c);
    }
    default:  return -38;               /* ENOSYS */
    }
}

/* Build argv[] (ASCII) from a space-separated string into a pool buffer.
 * Returns argc; *argvp points to the array, *poolp is the allocation to free. */
static int build_argv(char *rest, char ***argvp, VOID **poolp) {
    /* Allocate exactly one 4 KiB page (page-aligned) so that the single page
     * el0_co_setup() grants to EL0 covers both the argv[] array (front) and the
     * copied token strings (back). This is mandatory for the EL0 coroutine
     * path: a coroutine child runs unprivileged and may ONLY touch memory the
     * kernel has explicitly granted. If argv[] entries pointed at the kernel's
     * own command-line buffer the child would take a permission fault the
     * instant it dereferenced argv[0] (a confused-deputy the isolation is
     * designed to catch). So we copy each token into the pool here. */
    EFI_PHYSICAL_ADDRESS pa = 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &pa)) || !pa) return 0;
    char *base = (char *)(UINTN)pa;
    char **argv = (char **)(base);
    char *store = base + 4096;          /* token strings grow downward */
    int argc = 0;
    const char *p = (const char *)rest;
    while (*p && argc < MAXFD - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - start);
        if (len > 4095) len = 4095;
        store -= (len + 1);
        for (int i = 0; i < len; i++) store[i] = start[i];
        store[len] = 0;
        argv[argc++] = store;
    }
    argv[argc] = NULL;
    *argvp = argv; *poolp = (VOID *)(UINTN)pa;
    return argc;
}

/* `open <file> [args...]` */
static void cmd_open(char *rest) {
    /* skip leading spaces to find filename */
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest == 0) { con_print_ascii("open: missing file\r\n"); return; }

    /* filename = first token */
    char *p = rest;
    while (*p && *p != ' ' && *p != '\t') p++;
    int fnlen = (int)(p - rest);
    char fname[256];
    for (int i = 0; i < fnlen && i < 255; i++) fname[i] = rest[i];
    fname[fnlen] = 0;
    /* rest now points at args (skip the delimiter) */
    if (*p) p++;
    while (*p == ' ' || *p == '\t') p++;

    /* open & read the ELF */
    EFI_FILE_PROTOCOL *f = NULL;
    if (open_rel(fname, EFI_FILE_MODE_READ, 0, &f) != 0) {
        con_print_ascii("open: cannot open "); con_print_ascii(fname); con_write_bytes("\n", 1); return;
    }
    EFI_GUID infoId = EFI_FILE_INFO_ID;
    UINT8 ib[128]; UINTN isz = sizeof(ib);
    UINT64 fsize = 0;
    if (!EFI_ERROR(f->GetInfo(f, &infoId, &isz, ib))) fsize = ((EFI_FILE_INFO *)ib)->FileSize;
    if (fsize == 0) { f->Close(f); con_print_ascii("open: empty file\r\n"); return; }

    VOID *img = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 16, &img))) {
        f->Close(f); con_print_ascii("open: out of memory\r\n"); return;
    }
    UINTN rs = (UINTN)fsize;
    if (EFI_ERROR(f->Read(f, &rs, img))) {
        gBS->FreePool(img); f->Close(f); con_print_ascii("open: read error\r\n"); return;
    }
    f->Close(f);

    UINT64 entry, base, pages, sp;
    int r = elf_load((const uint8_t *)img, (UINT64)fsize, &entry, &base, &pages, &sp);
    g_parent_base = base; g_parent_end = base + pages * 4096ULL;
    if (r != 0) {
        gBS->FreePool(img);
        con_print_ascii("open: ELF load failed (");
        con_print_dec((UINT64)r);
        con_write_bytes(")\n", 1);
        return;
    }

    /* argv: pass the full rest (incl. program name as argv[0]) */
    char **argvp; VOID *pool;
    int argc = build_argv(rest, &argvp, &pool);

    for (int i = 0; i < MAXFD; i++) g_fds[i] = NULL;
    g_elf_exited = 0; g_elf_rc = 0;

    con_print_ascii("open: running "); con_print_ascii(fname); con_write_bytes("\n", 1);

    UINT64 rc = call_elf(entry, (UINT64)argc, (UINT64)argvp, 0,
                         (UINT64)sys_dispatch, sp);

    /* Restore the text console after a graphical userland program exits. */
    if (gST && gST->ConOut) gST->ConOut->ClearScreen(gST->ConOut);

    long code = g_elf_exited ? g_elf_rc : (long)rc;
    con_print_ascii("open: program exited with code ");
    con_print_dec((UINT64)code);
    con_write_bytes("\n", 1);

    gBS->FreePool(pool);
    gBS->FreePages(base, pages);
    gBS->FreePool(img);
    for (int i = 0; i < MAXFD; i++) if (g_fds[i]) { g_fds[i]->Close(g_fds[i]); g_fds[i] = NULL; }
}

/* ===================================================================
 * Shell
 * =================================================================== */
#define MAXTOK 16
static int tokenize(char *line, char *tok[], int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
    }
    return n;
}

static void shell_loop(void) {
    char line[256];
    char *tok[MAXTOK];
    for (;;) {
        con_print_ascii("\r\n");
        con_print_ascii(gCwd);
        con_print_ascii("> ");
        int n = con_read_bytes(line, sizeof(line) - 1);
        if (n <= 0) continue;
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        int tc = tokenize(line, tok, MAXTOK);
        if (tc == 0) continue;
        char *cmd = tok[0];
        if      (my_strcmp(cmd, "help") == 0)  cmd_help();
        else if (my_strcmp(cmd, "ls") == 0)     cmd_ls();
        else if (my_strcmp(cmd, "pwd") == 0)    cmd_pwd();
        else if (my_strcmp(cmd, "cls") == 0)    cmd_cls();
        else if (my_strcmp(cmd, "shutdown") == 0) cmd_shutdown();
        else if (my_strcmp(cmd, "color") == 0)    cmd_color(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "cd") == 0)     cmd_cd(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "mkdir") == 0)  cmd_mkdir(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "del") == 0)    cmd_del(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "copy") == 0) {
            if (tc >= 3) cmd_copy(tok[1], tok[2]);
            else con_print_ascii("copy: usage: copy <src> <dst>\r\n");
        }
        else if (my_strcmp(cmd, "rename") == 0) {
            if (tc >= 3) cmd_rename(tok[1], tok[2]);
            else con_print_ascii("rename: usage: rename <old> <new>\r\n");
        }
        else if (my_strcmp(cmd, "open") == 0) {
            /* reconstruct "file args..." from tokens */
            char rest[256]; int ri = 0;
            for (int i = 1; i < tc && ri < 250; i++) {
                if (i > 1) rest[ri++] = ' ';
                for (char *q = tok[i]; *q && ri < 250; q++) rest[ri++] = *q;
            }
            rest[ri] = 0;
            cmd_open(rest);
        }
        else if (my_strcmp(cmd, "tdesktop") == 0) {
            g_kcon_enable = 0;            /* hand the framebuffer back to the GUI */
            cmd_open("tdesktop.elf");
        }
        else if (my_strcmp(cmd, "gui") == 0) {
            g_kcon_enable = 0;            /* hand the framebuffer back to the GUI */
            cmd_open("gui.elf");
        }
        else if (my_strcmp(cmd, "time") == 0) {
            cmd_time();
        }
        else if (my_strcmp(cmd, "fetch") == 0) {
            cmd_fetch(tc > 1 ? tok[1] : "");
        }
        else if (my_strcmp(cmd, "nettest") == 0) {
            char rbuf[1100];
            int n = nw_net_selftest(rbuf, (int)sizeof(rbuf) - 1);
            if (n < 0) n = 0;
            rbuf[n] = 0;
            con_print_ascii("nettest: "); con_print_ascii(rbuf); con_write_bytes("\n", 1);
        }
        else if (my_strcmp(cmd, "sndtest") == 0) {
            cmd_sndtest();
        }
        else if (my_strcmp(cmd, "chinese") == 0) {
            con_write_bytes("\r\n中文显示测试：你好，世界！hudos 现在支持中文显示了。\r\n",
                            my_strlen("\r\n中文显示测试：你好，世界！hudos 现在支持中文显示了。\r\n"));
        }
        else if (my_strcmp(cmd, "fbcon") == 0) {
            if (g_kcon_enable) {
                g_kcon_enable = 0;
                con_print_ascii("fbcon: framebuffer console off (GUI owns the screen)\r\n");
            } else {
                g_kcon_enable = 1;
                kcon_clear();
                con_write_bytes("\r\n帧缓冲中文控制台已开启 —— hudos framebuffer console (CJK ready)\r\n",
                                my_strlen("\r\n帧缓冲中文控制台已开启 —— hudos framebuffer console (CJK ready)\r\n"));
            }
        }
        else if (my_strcmp(cmd, "mount") == 0) {
            if (tc >= 3) {
                int r = mount_do(tok[1], tok[2]);
                if (r == 0) con_print_ascii("mount: ok\r\n");
                else { con_print_ascii("mount: failed ("); con_print_dec((UINT64)(-r)); con_print_ascii(")\r\n"); }
            } else {
                char buf[1024]; mount_list_text(buf, (int)sizeof(buf));
                con_print_ascii(buf);
            }
        }
        else if (my_strcmp(cmd, "umount") == 0) {
            if (tc >= 2) {
                int r = umount_do(tok[1]);
                if (r == 0) con_print_ascii("umount: ok\r\n");
                else { con_print_ascii("umount: failed ("); con_print_dec((UINT64)(-r)); con_print_ascii(")\r\n"); }
            } else con_print_ascii("umount: usage: umount <dir>\r\n");
        }
        else {
            con_print_ascii("unknown command: "); con_print_ascii(cmd);
            con_print_ascii(" (try 'help')\r\n");
        }
    }
}

/* Read the "boot=" autostart preference from /settings.cfg (written by
 * tdesktop's Settings dialog). Returns 1 to launch gui.elf, 0 (default) to
 * launch tdesktop.elf. Tolerant of a missing/unreadable file. */
static int read_boot_pref(void) {
    EFI_FILE_PROTOCOL *f = NULL;
    if (open_rel("settings.cfg", EFI_FILE_MODE_READ, 0, &f) != 0 || !f) return 0;
    char buf[256]; UINTN got = sizeof(buf);   /* in: buffer size; out: bytes read */
    f->Read(f, &got, buf);
    f->Close(f);
    if (got == 0) return 0;
    if (got > (UINTN)sizeof(buf) - 1) got = (UINTN)sizeof(buf) - 1;
    buf[got] = 0;
    for (UINTN i = 0; i + 5 <= got; i++) {
        if (buf[i]=='b' && buf[i+1]=='o' && buf[i+2]=='o' && buf[i+3]=='t' && buf[i+4]=='=') {
            int v = 0; UINTN j = i + 5;
            while (j < got && buf[j] >= '0' && buf[j] <= '9') { v = v*10 + (buf[j]-'0'); j++; }
            if (v == 0 || v == 1) return v;
        }
    }
    return 0;
}

/* ===================================================================
 * Auto-test (runs if a file "AUTOTEST" exists at root)
 * =================================================================== */
static int run_autotest(void) {
    EFI_FILE_PROTOCOL *t = NULL;
    if (open_rel("AUTOTEST", EFI_FILE_MODE_READ, 0, &t) != 0) return 0;
    t->Close(t);

    g_kcon_enable = 1;          /* in autotest mode the shell is the primary console */
    kcon_clear();
    con_print_ascii("===== AUTOTEST =====\r\n");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[mkdir testdir]\r\n"); cmd_mkdir("testdir");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[cd testdir; pwd]\r\n"); cmd_cd("testdir"); cmd_pwd();
    con_print_ascii("[cd ..; pwd]\r\n"); cmd_cd(".."); cmd_pwd();
    con_print_ascii("[copy app/hello.elf copied.elf]\r\n"); cmd_copy("app/hello.elf", "copied.elf");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[open app/hello.elf]\r\n"); cmd_open("app/hello.elf");
    con_print_ascii("[open app/echo.elf one two three]\r\n"); cmd_open("app/echo.elf one two three");
    con_print_ascii("[open app/calc.elf]\r\n"); cmd_open("app/calc.elf");
    con_print_ascii("[del copied.elf]\r\n"); cmd_del("copied.elf");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[sndtest]\r\n"); cmd_sndtest();
    con_print_ascii("===== AUTOTEST DONE =====\r\n");
    return 1;
}

/* ===================================================================
 * Entry point
 * =================================================================== */

/* Phase A: EL1 MMU identity-map + exception vector table (defined in mmu.c) */
void hudos_mmu_init(EFI_SYSTEM_TABLE *st, EFI_BOOT_SERVICES *bs);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable; gBS = SystemTable->BootServices; gRT = SystemTable->RuntimeServices;
    gImageHandle = ImageHandle;

    /* Phase A: turn on the MMU with an identity map of all RAM/MMIO from the
     * UEFI memory map, and install EL1 exception vectors. We stay in EL1.
     * If anything goes wrong the fault handler writes to the PL011 UART and
     * spins forever, so a hung boot will show "HUDOS MMU FAULT" on -serial. */
    hudos_mmu_init(gST, gBS);
    el0_init();

    /* Phase B: prove we can enter and leave EL0 before letting a real program
     * use it. If anything is wrong we simply stay at EL1 — nothing regresses. */

    /* Enable the FP/SIMD unit for the whole OS. On aarch64 the floating-point
     * and NEON unit is *trapped* by default: at EL2 via CPTR_EL2.TFP, and at
     * EL0 via CPACR_EL1.FPEN. hudos runs userland ELFs at the same privilege
     * level as the kernel, so until we clear these traps any userland program
     * that touches FP/SIMD (e.g. minimp3 inside mp3play.elf) faults with a
     * "Synchronous Exception". The kernel itself never uses FP, so enabling it
     * only un-traps userland — safe. */
    {
        __asm__ volatile (
            "mrs x0, currentel\n"
            "cmp x0, #0x8\n"          /* CurrentEL[3:2]==0b10 -> EL2 */
            "b.ne 1f\n"
            "msr cptr_el2, xzr\n"     /* EL2: clear TFP/TCPAC so EL1/EL0 may use FP/SIMD */
            "1:\n"
            "mov x0, #0x300000\n"     /* CPACR_EL1.FPEN = 0b11: EL0+EL1 may use FP/SIMD */
            "msr cpacr_el1, x0\n"
            "isb\n"
            ::: "x0", "memory"
        );
    }

    {
        int rc = el0_selftest();
        if (rc == 0) con_print_ascii("el0: OK, user mode available\r\n");
        else {
            con_print_ascii("el0: self-test FAILED, staying at EL1\r\n");
            g_el0_enable = 0;
        }
    }

    con_print_ascii("hudos-server 0.1  (aarch64 UEFI Unix-like shell)\r\n");
    con_print_ascii("(c) HUDOS project\r\n");

    if (find_root() != 0) {
        con_print_ascii("FATAL: no ESP filesystem found\r\n");
        return EFI_LOAD_ERROR;
    }
    con_print_ascii("filesystem: ESP mounted at / \r\n");

    disk_enum();   /* enumerate whole-disk block devices for partition management */

    gfx_try_init();



    gConIn = gST->ConIn;
    if (gConIn) gConIn->Reset(gConIn, FALSE);

    /* Try to open the Extended console-input protocol so we can query the
     * live modifier-key (Ctrl/Shift/Alt) state via GetState().  Not all
     * firmware expose it; if unavailable we simply fall back to 0. */
    {
        EFI_GUID exGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
        if (gBS && gST && !EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle,
                                                         &exGuid, (VOID **)&gConInEx)))
            ;   /* got it */
        else
            gConInEx = NULL;
    }

    int autotest = run_autotest();

    /* Auto-launch the graphical desktop on a normal boot. In automated
       (autotest) mode we skip it so the text shell stays available for
       headless validation. */
    if (!autotest) {
        if (read_boot_pref()) cmd_open("gui.elf");
        else                   cmd_open("tdesktop.elf");
    }

    shell_loop();
    return EFI_SUCCESS;
}
