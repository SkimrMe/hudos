#ifndef HUDOS_SERVER_SYSCALL_H
#define HUDOS_SERVER_SYSCALL_H

/* The loader passes a syscall dispatch function pointer (in x3 at _start).
 * Userland calls it with the Linux aarch64 syscall numbers below. */
typedef long (*syscall_t)(long, long, long, long, long, long);
extern syscall_t __syscall;

long syscall(long n, long a, long b, long c, long d, long e);

/* process / IO */
void exit(int code);
int  write(int fd, const char *buf, int n);
int  read(int fd, char *buf, int n);
int  open(const char *path, int flags, int mode);
int  close(int fd);
long lseek(int fd, long off, int whence);

/* convenience */
void print_str(const char *s);
void print_dec(long v);

/* desktop integration: run an app from a terminal window / list a directory */
int  exec_app(const char *path, char *outbuf, int outcap);
int  exec_argv(const char *cmdline, char *outbuf, int outcap);  /* run with argv */
int  list_dir(const char *path, char *outbuf, int outcap);

/* interactive Terminal coroutine control */
int  exec_co(const char *path, char *outbuf, int outcap);
int  exec_co_argv(const char *cmdline, char *outbuf, int outcap);  /* run with argv */
int  co_resume(void);
int  co_push(const char *buf, int n);
int  co_kill(void);
int  co_out_len(void);
int  co_out_append(const char *buf, int n);
int  co_set_yield(int on);   /* enable per-frame yielding for gui-launched children */

/* networking */
int  sys_fetch(const char *url, char *buf, int cap);   /* HTTP GET; ret body len / -1 */

/* graphics / input extensions */
int  gop_info(unsigned long *info);
int  gop_fill(int x, int y, int w, int h, unsigned rgb);
int  gop_pix(int x, int y, unsigned rgb);
int  gop_present(void);        /* SYS_gop_present: flush shadow -> screen */
int  mouse_get(int *m);
int  kbd_poll(void);
int  kbd_get(int *k);          /* k[2] = {unicode, scancode}; ret 1 if a key was read */
int  kbd_get_ex(int *k);        /* k[3] = {unicode, scancode, shiftstate}; per-event mods; ret 1 if key */
int  kbd_mods(void);            /* ret modifier shift-state (EFI KeyShiftState); 0 if unavailable */
void sleep_ms(int ms);

/* TUI (text-console) extensions */
int  con_size(int *dims);      /* dims[2] = {cols, rows} */
void con_clear(void);
void con_put(int col, int row, char ch, int attr);
void con_cursor(int visible);  /* 1 = show hardware cursor, 0 = hide */

/* Linux aarch64 syscall numbers (subset) */
#define SYS_write   64
#define SYS_read    63
#define SYS_openat  56
#define SYS_close   57
#define SYS_lseek   62
#define SYS_exit    93
#define SYS_exit_group 94

/* hudos-server graphics / input extensions */
#define SYS_gop_info   400   /* info[5] = {base,w,h,stride,fmt}; ret 0/-1 */
#define SYS_gop_fill   401   /* gop_fill(x,y,w,h,rgb) */
#define SYS_gop_pix    402   /* gop_pix(x,y,rgb) */
#define SYS_mouse      403   /* m[4] = {x,y,buttons,clicked} */
#define SYS_kbd_poll   404   /* non-blocking key poll; unicode char or 0 */
#define SYS_sleep      405   /* sleep(ms) */
#define SYS_kbd_get    406   /* kbd_get(a = int[2] {unicode,scancode}); ret 1 if key else 0 */
#define SYS_kbd_get_ex 441   /* kbd_get_ex(a = int[3] {unicode,scancode,shiftstate}); per-event mods; ret 1 if key */
#define SYS_kbd_mods   419   /* kbd_mods(): return modifier shift-state (EFI KeyShiftState) */
#define SYS_gop_present 442  /* gop_present(): flush shadow -> screen (Blt-only GOP) */
#define SYS_con_size   407   /* con_size(a = int[2] {cols,rows}) */
#define SYS_con_clear  408   /* con_clear() */
#define SYS_con_put    409   /* con_put(col,row,ch,attr) */
#define SYS_con_cursor 410   /* con_cursor(visible) */

/* hudos-server desktop integration (tdesktop "Applications") */
#define SYS_exec       411   /* exec_app(path, outbuf, outcap): load+run ELF,
                                capture stdout into outbuf; ret captured bytes / -1 */
#define SYS_exec_argv  425   /* exec_argv(cmdline, outbuf, outcap): load+run ELF
                                with argv parsed from `cmdline` (space-separated);
                                e.g. "app/imgview.elf picture/x.png"; ret bytes/-1 */
#define SYS_listdir    412   /* list_dir(path, outbuf, outcap): enumerate dir,
                                NUL-separated names into outbuf; ret entry count / -1 */
/* interactive Terminal coroutine (lets programs read stdin) */
#define SYS_exec_co    413   /* exec_co(path, outbuf, outcap): start ELF as a
                                coroutine; ret 0/-1. Drive via co_resume. */
#define SYS_exec_co_argv 426 /* exec_co_argv(cmdline, outbuf, outcap): like 413
                                but parse argv from the command line. */
#define SYS_co_resume  414   /* co_resume(): run child until it blocks/exits;
                                ret state (2 blocked / 3 done) */
#define SYS_co_push    415   /* co_push(buf, n): append bytes to child stdin */
#define SYS_co_kill    416   /* co_kill(): terminate a running child + free it */
#define SYS_co_outlen  417   /* co_out_len(): captured stdout bytes so far */
#define SYS_co_outappend 418 /* co_out_append(buf, n): append bytes to the
                                captured-output buffer only (GUI local echo) */
#define SYS_co_set_yield 428  /* co_set_yield(on): per-frame yield for gui children */

/* networking: fetch a URL over HTTP (no TLS) and return the body */
#define SYS_fetch     420    /* sys_fetch(url, buf, cap): HTTP GET; ret body len / -1 */

/* sound: minimal Intel HDA playback (44.1 kHz / 16-bit / mono PCM) */
#define SYS_audio_init 422   /* sys_audio_init(): probe + init HDA; ret 0/-errno */
#define SYS_audio_play 423   /* sys_audio_play(buf, len): play PCM; ret 0/-errno */
#define SYS_unlink    424   /* unlink(path): delete a file/dir (EFI Delete); ret 0/-1 */
#define SYS_copy      427   /* copy(src, dst): recursive copy of file/dir; ret 0/-1 */
#define SYS_mount     430   /* mount(volspec, dir): mount a volume at a directory; ret 0/-err */
#define SYS_umount    431   /* umount(dir): unmount a directory; ret 0/-err */
#define SYS_mount_list 432  /* mount_list(buf, cap): fill buf with volume/mount text; ret len/-1 */
#define SYS_mkdir     433   /* mkdir(path): create dir (and parents) via fs_mkdir_p; ret 0/-1 */
#define SYS_volume_info 434 /* volume_info(index, info_ptr, size): index<0 => ret count;
                               else fill vol_info_t; ret 0/-1 */
#define SYS_format      435 /* format(index, label): format a non-system volume as FAT16; ret 0/-err */

/* disk / partition management (whole-disk level, below the volume layer) */
#define SYS_disk_list        436 /* disk_list(buf, max): fill disk_info_t[]; ret count/-1 */
#define SYS_disk_parts       437 /* disk_parts(disk, buf, cap): fill part_info_t[]; ret count/-1 */
#define SYS_disk_part_create 438 /* disk_part_create(disk, size_sectors, type): ret slot/-err */
#define SYS_disk_part_delete 439 /* disk_part_delete(disk, part_idx): ret 0/-err */
#define SYS_disk_part_format 440 /* disk_part_format(disk, part_idx, label): ret 0/-err */

/* layout MUST match the kernel's k_disk_info exactly (U64=8B, U32=4B, U8=1B, align 8) */
typedef struct {
    unsigned long long total;   /* total bytes */
    unsigned int       block;   /* bytes per block */
    unsigned char      is_sys;  /* 1 = system (boot) disk, not partitionable */
    unsigned char      _pad[3];
} disk_info_t;
/* layout MUST match the kernel's k_part_info exactly */
typedef struct {
    unsigned long long start;   /* start sector */
    unsigned long long size;    /* size in sectors */
    unsigned int       type;    /* MBR partition type byte */
    unsigned char      active;  /* boot flag (0x80) */
    unsigned char      used;    /* 1 = occupied */
    unsigned char      _pad[2];
} part_info_t;
int  disk_list(disk_info_t *buf, int max);
int  disk_parts(int disk, part_info_t *buf, int cap);
int  disk_part_create(int disk, long size_sectors, int type);
int  disk_part_delete(int disk, int part_idx);
int  disk_part_format(int disk, int part_idx, const char *label);

/* disk management: layout MUST match the kernel's vol_info_t exactly
 * (UINT64=8B, UINT32=4B, UINT8=1B, struct aligned to 8B). */
typedef struct {
    unsigned long long total;   /* total bytes          */
    unsigned long long free;    /* free bytes           */
    unsigned long long used;    /* used bytes           */
    unsigned int       block;   /* bytes per block      */
    unsigned char      is_sys;  /* 1 = system (boot) volume, not formattable */
    unsigned char      can_fmt; /* 1 = raw block device present & not system => formattable */
    unsigned char      _pad[2];
    char               label[16];
} vol_info_t;
int  volume_info(int index, vol_info_t *info, int size);   /* index<0 => ret count; else 0/-1 */
int  format_volume(int index, const char *label);          /* ret 0 / -err */

/* sound wrappers (defined in syscall.c) */
long sys_audio_init(void);
long sys_audio_play(const void *buf, long len);
int  unlink(const char *path);   /* delete a file/directory; ret 0 on success, -1 on error */
int  copy(const char *src, const char *dst);   /* recursive copy of file/dir; ret 0/-1 */
int  mkdir(const char *path);     /* create a directory (and parents); ret 0 on success, -1 on error */
int  mount(const char *vol, const char *dir);   /* mount a volume (index or label) at a directory */
int  umount(const char *dir);                  /* unmount a directory */
int  mount_list(char *buf, int cap);            /* fill buf with volume/mount table text */

/* open flags */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400

/* lseek whence */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

#define STDIN_FD   0
#define STDOUT_FD  1
#define STDERR_FD  2

#endif
