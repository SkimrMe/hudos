#include "syscall.h"

syscall_t __syscall;

/* EL0 transport (Linux aarch64 convention): nr in x8, args in x0-x4, rv in x0. */
static long svc_call(long n, long a, long b, long c, long d, long e)
{
    register long r0 __asm__("x0") = a;
    register long r1 __asm__("x1") = b;
    register long r2 __asm__("x2") = c;
    register long r3 __asm__("x3") = d;
    register long r4 __asm__("x4") = e;
    register long r8 __asm__("x8") = n;
    __asm__ volatile ("svc #0"
                      : "+r" (r0)
                      : "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r8)
                      : "memory", "cc");
    return r0;
}

long syscall(long n, long a, long b, long c, long d, long e) {
    /* The loader picks the privilege level and passes the choice in x3:
     *   non-zero -> EL1 legacy mode: direct call into the kernel
     *   zero     -> we run at EL0 and must trap through SVC
     * Supporting both lets each program migrate to EL0 independently. */
    if (__syscall) return __syscall(n, a, b, c, d, e);
    return svc_call(n, a, b, c, d, e);
}

void exit(int code) { syscall(SYS_exit, code, 0, 0, 0, 0); }
int  write(int fd, const char *buf, int n) { return (int)syscall(SYS_write, fd, (long)buf, n, 0, 0); }
int  read(int fd, char *buf, int n)        { return (int)syscall(SYS_read, fd, (long)buf, n, 0, 0); }
int  open(const char *path, int flags, int mode) { return (int)syscall(SYS_openat, -100, (long)path, flags, mode, 0); }
int  close(int fd) { return (int)syscall(SYS_close, fd, 0, 0, 0, 0); }
long lseek(int fd, long off, int whence) { return syscall(SYS_lseek, fd, off, whence, 0, 0); }

/* graphics / input */
int  gop_info(unsigned long *info) { return (int)syscall(SYS_gop_info, (long)info, 0, 0, 0, 0); }
int  gop_fill(int x, int y, int w, int h, unsigned rgb) { return (int)syscall(SYS_gop_fill, x, y, w, h, rgb); }
int  gop_pix(int x, int y, unsigned rgb) { return (int)syscall(SYS_gop_pix, x, y, rgb, 0, 0); }
int  gop_present(void) { return (int)syscall(SYS_gop_present, 0, 0, 0, 0, 0); }
int  mouse_get(int *m) { return (int)syscall(SYS_mouse, (long)m, 0, 0, 0, 0); }
int  kbd_poll(void) { return (int)syscall(SYS_kbd_poll, 0, 0, 0, 0, 0); }
int  kbd_get(int *k) { return (int)syscall(SYS_kbd_get, (long)k, 0, 0, 0, 0); }
int  kbd_get_ex(int *k) { return (int)syscall(SYS_kbd_get_ex, (long)k, 0, 0, 0, 0); }
int  kbd_mods(void) { return (int)syscall(SYS_kbd_mods, 0, 0, 0, 0, 0); }
void sleep_ms(int ms) { syscall(SYS_sleep, ms, 0, 0, 0, 0); }

/* TUI (text-console) extensions */
int  con_size(int *dims) { return (int)syscall(SYS_con_size, (long)dims, 0, 0, 0, 0); }
void con_clear(void)     { syscall(SYS_con_clear, 0, 0, 0, 0, 0); }
void con_put(int col, int row, char ch, int attr) {
    syscall(SYS_con_put, col, row, (long)(unsigned char)ch, attr, 0);
}
void con_cursor(int visible) { syscall(SYS_con_cursor, visible, 0, 0, 0, 0); }

int exec_app(const char *path, char *outbuf, int outcap) {
    return (int)syscall(SYS_exec, (long)path, (long)outbuf, outcap, 0, 0);
}
int exec_argv(const char *cmdline, char *outbuf, int outcap) {
    return (int)syscall(SYS_exec_argv, (long)cmdline, (long)outbuf, outcap, 0, 0);
}
int list_dir(const char *path, char *outbuf, int outcap) {
    return (int)syscall(SYS_listdir, (long)path, (long)outbuf, outcap, 0, 0);
}

int exec_co(const char *path, char *outbuf, int outcap) {
    return (int)syscall(SYS_exec_co, (long)path, (long)outbuf, outcap, 0, 0);
}
int exec_co_argv(const char *cmdline, char *outbuf, int outcap) {
    return (int)syscall(SYS_exec_co_argv, (long)cmdline, (long)outbuf, outcap, 0, 0);
}
int co_resume(void) {
    return (int)syscall(SYS_co_resume, 0, 0, 0, 0, 0);
}
int co_push(const char *buf, int n) {
    return (int)syscall(SYS_co_push, (long)buf, n, 0, 0, 0);
}
int co_kill(void) {
    return (int)syscall(SYS_co_kill, 0, 0, 0, 0, 0);
}
int co_out_len(void) {
    return (int)syscall(SYS_co_outlen, 0, 0, 0, 0, 0);
}
int co_out_append(const char *buf, int n) {
    return (int)syscall(SYS_co_outappend, (long)buf, n, 0, 0, 0);
}
int co_set_yield(int on) {
    return (int)syscall(SYS_co_set_yield, on, 0, 0, 0, 0);
}

int sys_fetch(const char *url, char *buf, int cap) {
    return (int)syscall(SYS_fetch, (long)url, (long)buf, cap, 0, 0);
}

long sys_audio_init(void) { return syscall(SYS_audio_init, 0,0,0,0,0); }
long sys_audio_play(const void *buf, long len) {
    return syscall(SYS_audio_play, (long)buf, len, 0,0,0);
}
int unlink(const char *path) {
    return (int)syscall(SYS_unlink, (long)path, 0, 0, 0, 0);
}
int copy(const char *src, const char *dst) {
    return (int)syscall(SYS_copy, (long)src, (long)dst, 0, 0, 0);
}
int mount(const char *vol, const char *dir) {
    return (int)syscall(SYS_mount, (long)vol, (long)dir, 0, 0, 0);
}
int umount(const char *dir) {
    return (int)syscall(SYS_umount, (long)dir, 0, 0, 0, 0);
}
int mount_list(char *buf, int cap) {
    return (int)syscall(SYS_mount_list, (long)buf, cap, 0, 0, 0);
}
int mkdir(const char *path) {
    return (int)syscall(SYS_mkdir, (long)path, 0, 0, 0, 0);
}
int volume_info(int index, vol_info_t *info, int size) {
    return (int)syscall(SYS_volume_info, (long)index, (long)info, (long)size, 0, 0);
}
int format_volume(int index, const char *label) {
    return (int)syscall(SYS_format, (long)index, (long)label, 0, 0, 0);
}

int disk_list(disk_info_t *buf, int max) {
    return (int)syscall(SYS_disk_list, (long)buf, (long)max, 0, 0, 0);
}
int disk_parts(int disk, part_info_t *buf, int cap) {
    return (int)syscall(SYS_disk_parts, (long)disk, (long)buf, (long)cap, 0, 0);
}
int disk_part_create(int disk, long size_sectors, int type) {
    return (int)syscall(SYS_disk_part_create, (long)disk, (long)size_sectors, (long)type, 0, 0);
}
int disk_part_delete(int disk, int part_idx) {
    return (int)syscall(SYS_disk_part_delete, (long)disk, (long)part_idx, 0, 0, 0);
}
int disk_part_format(int disk, int part_idx, const char *label) {
    return (int)syscall(SYS_disk_part_format, (long)disk, (long)part_idx, (long)label, 0, 0);
}

void print_str(const char *s) {
    int n = 0; while (s[n]) n++;
    write(STDOUT_FD, s, n);
}

void print_dec(long v) {
    char buf[24];
    int i = 0;
    if (v < 0) { write(STDOUT_FD, "-", 1); v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t; }
    write(STDOUT_FD, buf, i);
}
