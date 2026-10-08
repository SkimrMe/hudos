#ifndef GAMES_COMMON_H
#define GAMES_COMMON_H

/* hudos port of the 游戏资源 4-game framework.
 * This header carries the original game API contract (common.h) plus the tiny
 * libc helpers that games_adapter.c provides (the freestanding toolchain has
 * no libc). */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int i32;

extern int sw;
extern int sh;
extern u32 *fb;

void sys_init(void);
void sys_flip(void);
void sys_poll(void);
void sys_sleep(int ms);
u32 sys_ticks(void);
int sys_rand(void);
int sys_mx(void);
int sys_my(void);
int sys_ml(void);
int sys_mr(void);
int sys_key(void);
int sys_read(const char *p, void *b, int n);
int sys_write(const char *p, const void *b, int n);
int sys_read_off(const char *p, void *b, int n, long off);

void px(int x, int y, u32 c);
u32 gx(int x, int y);
void fb_copy(int x, int y, const u32 *src, int w, int h);
void rect(int x, int y, int w, int h, u32 c);
void box(int x, int y, int w, int h, int down);
void bmp(const char *name, int x, int y);
int bmpw(const char *name);
int bmph(const char *name);
void text(int x, int y, const char *s, u32 c, int lang);
void load_font(void);
void set_cjk_size(int n);   /* 16 = compact (legacy), 32 = full-res crisp (gui); 现支持 12..48 任意值 */
int  cjk_size(void);
int  ascii_w(void);         /* ASCII 当前字宽（逻辑 px） */
int  ascii_h(void);         /* ASCII 当前字高（逻辑 px） */
void set_ascii_size(int h); /* 设 ASCII 目标高度（字宽=高度/2），默认 16 */
void set_bold(int on);      /* 字体样式：0=常规 1=粗体 */

void mine_run(void);
void spider_run(void);
void tetris_run(void);
void ball3d_run(void);
void main_menu(void);
void gui_run(void);

/* rank persistence (games_rank.c) */
typedef struct {
    char game[16];
    char cn[32];
    char en[32];
    int score;
    int level;
    long time;
} Rank;
int  rank_load(const char *game, Rank *out, int max);
void rank_save(const char *game, const char *cn, const char *en, int score, int level, long time);
void rank_show(const char *game, int lang);

/* ---- tiny libc (provided by games_adapter.c) ---- */
void *memset(void *d, int v, int n);
void *memcpy(void *d, const void *s, int n);
int   memcmp(const void *a, const void *b, int n);
int   strlen(const char *s);
char *strcpy(char *d, const char *s);
int   strcmp(const char *a, const char *b);
char *strncpy(char *d, const char *s, int n);
char *strcat(char *d, const char *s);
char *strchr(char *s, int c);
char *my_itoa(long n, char *buf);   /* returns buf; writes decimal string */

#endif
