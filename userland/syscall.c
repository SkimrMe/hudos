#include "syscall.h"

syscall_t __syscall;

long syscall(long n, long a, long b, long c, long d, long e) {
    return __syscall(n, a, b, c, d, e);
}

void exit(int code) { syscall(SYS_exit, code, 0, 0, 0, 0); }
int  write(int fd, const char *buf, int n) { return (int)syscall(SYS_write, fd, (long)buf, n, 0, 0); }
int  read(int fd, char *buf, int n)        { return (int)syscall(SYS_read, fd, (long)buf, n, 0, 0); }
int  open(const char *path, int flags, int mode) { return (int)syscall(SYS_openat, -100, (long)path, flags, mode, 0); }
int  close(int fd) { return (int)syscall(SYS_close, fd, 0, 0, 0, 0); }
long lseek(int fd, long off, int whence) { return syscall(SYS_lseek, fd, off, whence, 0, 0); }

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
