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

/* ---- globals ---- */
static EFI_SYSTEM_TABLE         *gST;
static EFI_BOOT_SERVICES        *gBS;
static EFI_RUNTIME_SERVICES     *gRT;
static EFI_HANDLE                gImageHandle;
static EFI_FILE_PROTOCOL        *gRoot = NULL;   /* ESP root directory */
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *gConIn = NULL;

/* current working directory, ASCII, Unix-style slash-separated, e.g. "/" or "/foo" */
static char gCwd[256] = "/";

/* ===================================================================
 * Console output (ConOut -> QEMU -serial)
 * =================================================================== */
static void con_print(const CHAR16 *s) {
    if (gST && gST->ConOut && gST->ConOut->OutputString)
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
}

/* Print ASCII bytes to the console, mapping '\n' -> CRLF. */
static void con_write_bytes(const char *buf, int n) {
    CHAR16 line[160];
    int i = 0;
    for (int k = 0; k < n; k++) {
        unsigned char ch = (unsigned char)buf[k];
        if (ch == '\n') { line[i++] = (CHAR16)'\r'; line[i++] = (CHAR16)'\n'; }
        else if (ch == '\r') { /* ignore, handled by \n */ }
        else if (ch == 0) { break; }
        else line[i++] = (CHAR16)ch;
        if (i >= 150) { line[i] = 0; con_print(line); i = 0; }
    }
    if (i > 0) { line[i] = 0; con_print(line); }
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
static void join_path(const char *cwd, const char *name, char *out){
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

/* Find the ESP: the SimpleFileSystem volume whose root contains \EFI. */
static int find_root(void) {
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN cnt = 0; EFI_HANDLE *hs = NULL;
    EFI_STATUS st = gBS->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &cnt, &hs);
    if (EFI_ERROR(st) || cnt == 0 || !hs) return -1;

    int found = 0;
    CHAR16 efi[] = {'\\','E','F','I',0};
    for (UINTN i = 0; i < cnt; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *cand = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &fsGuid, (VOID **)&cand))) continue;
        EFI_FILE_PROTOCOL *cr = NULL;
        if (EFI_ERROR(cand->OpenVolume(cand, &cr))) continue;
        EFI_FILE_PROTOCOL *t = NULL;
        if (!EFI_ERROR(cr->Open(cr, &t, efi, EFI_FILE_MODE_READ, 0))) {
            t->Close(t); gRoot = cr; found = 1; break;
        }
        cr->Close(cr);
    }
    if (!found && cnt > 0) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *cand = NULL;
        if (!EFI_ERROR(gBS->HandleProtocol(hs[0], &fsGuid, (VOID **)&cand)))
            if (!EFI_ERROR(cand->OpenVolume(cand, &gRoot))) found = 1;
    }
    gBS->FreePool(hs);
    return found ? 0 : -1;
}

/* Open a file/dir at (cwd + name) with the given mode. Returns 0 on success. */
static int open_rel(const char *name, UINT64 mode, UINT64 attr, EFI_FILE_PROTOCOL **out) {
    char full[300];
    join_path(gCwd, name, full);
    CHAR16 w[300];
    a2c_e(full, w, 300);
    EFI_STATUS st = gRoot->Open(gRoot, out, w, mode, attr);
    return EFI_ERROR(st) ? -1 : 0;
}

/* ===================================================================
 * Shell commands
 * =================================================================== */
static void cmd_help(void) {
    con_print_ascii(
        "hudos-server commands:\r\n"
        "  ls               list current directory\r\n"
        "  cd [dir]         change directory (.. , /abs , or name)\r\n"
        "  mkdir <name>     create a directory\r\n"
        "  del <name>       delete a file\r\n"
        "  copy <src> <dst> copy a file\r\n"
        "  open <file> [a]  load & run a Unix executable (aarch64 ELF)\r\n"
        "  pwd              print working directory\r\n"
        "  cls              clear screen\r\n"
        "  help             this help\r\n");
}

static void cmd_pwd(void) {
    con_print_ascii("PWD: ");
    con_print_ascii(gCwd);
    con_write_bytes("\n", 1);
}

static void cmd_cls(void) {
    if (gST->ConOut->ClearScreen) gST->ConOut->ClearScreen(gST->ConOut);
    else con_print_ascii("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
}

static void cmd_ls(void) {
    CHAR16 w[256]; a2c_e(gCwd, w, 256);
    EFI_FILE_PROTOCOL *d = NULL;
    if (EFI_ERROR(gRoot->Open(gRoot, &d, w, EFI_FILE_MODE_READ, 0))) {
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
    CHAR16 w[300]; a2c_e(cand, w, 300);
    EFI_FILE_PROTOCOL *t = NULL;
    EFI_STATUS st = gRoot->Open(gRoot, &t, w, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !t) { con_print_ascii("cd: "); con_print_ascii(arg); con_print_ascii(": no such directory\r\n"); return; }
    EFI_GUID infoId = EFI_FILE_INFO_ID;
    UINT8 ib[128]; UINTN isz = sizeof(ib);
    st = t->GetInfo(t, &infoId, &isz, ib);
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
    UINT64 stack_pages = 4;
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

static int alloc_fd(void) {
    for (int i = 3; i < MAXFD; i++) if (g_fds[i] == NULL) return i;
    return -1;
}

/* The dispatcher is referenced by address and called by userland via x3. */
static long sys_dispatch(long n, long a, long b, long c, long d, long e) {
    switch (n) {
    case 64: { /* write(fd, buf, count) */
        int fd = (int)a; const char *buf = (const char *)(UINTN)b; long count = c;
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
        if (fd >= 3 && g_fds[fd]) { g_fds[fd]->Close(g_fds[fd]); g_fds[fd] = NULL; return 0; }
        return -1;
    }
    case 56: { /* openat(dirfd, path, flags, mode) -- dirfd ignored, relative to cwd */
        const char *path = (const char *)(UINTN)b;
        long flags = c;
        UINT64 mode = EFI_FILE_MODE_READ;
        if (flags & 0x40) mode |= EFI_FILE_MODE_CREATE;   /* O_CREAT */
        if (flags & (0x01 | 0x02)) mode |= EFI_FILE_MODE_WRITE;  /* O_WRONLY | O_RDWR */
        char full[300]; join_path(gCwd, path, full);
        CHAR16 w[300]; a2c_e(full, w, 300);
        EFI_FILE_PROTOCOL *f = NULL;
        EFI_STATUS st = gRoot->Open(gRoot, &f, w, mode, 0);
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
        g_elf_exited = 1; g_elf_rc = a; return a;
    case 172: return 1;                 /* getpid */
    case 174:
    case 175: return 0;                 /* getuid / geteuid */
    case 214: return 0x60000000;        /* brk (no-op) */
    default:  return -38;               /* ENOSYS */
    }
}

/* Build argv[] (ASCII) from a space-separated string into a pool buffer.
 * Returns argc; *argvp points to the array, *poolp is the allocation to free. */
static int build_argv(char *rest, char ***argvp, VOID **poolp) {
    VOID *pool = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, 4096, &pool)) || !pool) return 0;
    char *base = (char *)pool;
    char **argv = (char **)(base);
    int argc = 0;
    char *p = rest;
    while (*p && argc < MAXFD - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    *argvp = argv; *poolp = pool;
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
        else if (my_strcmp(cmd, "cd") == 0)     cmd_cd(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "mkdir") == 0)  cmd_mkdir(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "del") == 0)    cmd_del(tc > 1 ? tok[1] : "");
        else if (my_strcmp(cmd, "copy") == 0) {
            if (tc >= 3) cmd_copy(tok[1], tok[2]);
            else con_print_ascii("copy: usage: copy <src> <dst>\r\n");
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
        else {
            con_print_ascii("unknown command: "); con_print_ascii(cmd);
            con_print_ascii(" (try 'help')\r\n");
        }
    }
}

/* ===================================================================
 * Auto-test (runs if a file "AUTOTEST" exists at root)
 * =================================================================== */
static void run_autotest(void) {
    EFI_FILE_PROTOCOL *t = NULL;
    if (open_rel("AUTOTEST", EFI_FILE_MODE_READ, 0, &t) != 0) return;
    t->Close(t);

    con_print_ascii("===== AUTOTEST =====\r\n");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[mkdir testdir]\r\n"); cmd_mkdir("testdir");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[cd testdir; pwd]\r\n"); cmd_cd("testdir"); cmd_pwd();
    con_print_ascii("[cd ..; pwd]\r\n"); cmd_cd(".."); cmd_pwd();
    con_print_ascii("[copy hello.elf copied.elf]\r\n"); cmd_copy("hello.elf", "copied.elf");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("[open hello.elf]\r\n"); cmd_open("hello.elf");
    con_print_ascii("[open echo.elf one two three]\r\n"); cmd_open("echo.elf one two three");
    con_print_ascii("[open calc.elf]\r\n"); cmd_open("calc.elf");
    con_print_ascii("[open copied.elf]\r\n"); cmd_open("copied.elf");
    con_print_ascii("[del copied.elf]\r\n"); cmd_del("copied.elf");
    con_print_ascii("[ls root]\r\n"); cmd_ls();
    con_print_ascii("===== AUTOTEST DONE =====\r\n");
}

/* ===================================================================
 * Entry point
 * =================================================================== */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable; gBS = SystemTable->BootServices; gRT = SystemTable->RuntimeServices;
    gImageHandle = ImageHandle;

    con_print_ascii("hudos-server 0.1  (aarch64 UEFI Unix-like shell)\r\n");
    con_print_ascii("(c) HUDOS project\r\n");

    if (find_root() != 0) {
        con_print_ascii("FATAL: no ESP filesystem found\r\n");
        return EFI_LOAD_ERROR;
    }
    con_print_ascii("filesystem: ESP mounted at / \r\n");

    gConIn = gST->ConIn;
    if (gConIn) gConIn->Reset(gConIn, FALSE);

    run_autotest();

    shell_loop();
    return EFI_SUCCESS;
}
