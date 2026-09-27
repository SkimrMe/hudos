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

/* Linux aarch64 syscall numbers (subset) */
#define SYS_write   64
#define SYS_read    63
#define SYS_openat  56
#define SYS_close   57
#define SYS_lseek   62
#define SYS_exit    93
#define SYS_exit_group 94

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
