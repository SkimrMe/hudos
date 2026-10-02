#ifndef HUDOS_WIN_H
#define HUDOS_WIN_H

/* 可复用窗口 / 鼠标工具（Win3.2 风格）。实现见 games_adapter.c。
 *
 * 仅 gui / filemgr 这类 GUI 程序包含本头文件；guishell.c 自带同名
 * static void draw_cursor(void)，为避免冲突，win_frame / draw_cursor 不放进
 * common.h，单独放在这里。
 */

/* 矩形（按钮命中测试用） */
typedef struct { int x, y, w, h; } WinBtn;

/* win_frame() 设置的可点击按钮矩形，调用方可以用来做命中测试 */
extern WinBtn g_win_close;   /* 标题栏右侧关闭按钮 */
extern WinBtn g_win_sys;     /* 标题栏左侧系统菜单按钮 */
extern WinBtn g_win_min;     /* 标题栏右侧最小化按钮 */

/* 画一个统一外观的窗口：外黑边 + 灰面 + 海军蓝标题栏 + 系统菜单/最小化/关闭按钮。
 * 调用方在 (x+1, y+1, w-2, h-2) 这块“客户区”内继续画自己的内容。 */
void win_frame(int x, int y, int w, int h, const char *title);

/* 画 Windows 风格白色箭头鼠标（逻辑坐标 mx,my 为箭头尖端）。
 * 任何接管整帧的程序（协程子程序）都必须在 sys_flip() 之前调用它，
 * 否则鼠标会“消失”（gui 每帧画、子程序若不画就看不见）。 */
void draw_cursor(int mx, int my);

/* ===================== 多窗口管理（可复用） =====================
 * 支持任意数量的浮动窗口：标题栏拖动移动、点击置顶（z-order 层级切换）。
 * 每个窗口是一块 (x,y,w,h) + 标题；内容由调用方通过 draw 回调绘制。
 */
typedef struct Win {
    int x, y, w, h;
    const char *title;
    int id;            /* 调用方自定义的窗口编号 */
    int open;          /* 1 可见, 0 隐藏 */
    int z;             /* z-order：越大越靠前（置顶） */
    WinBtn close;      /* 关闭按钮命中矩形（draw 时由 win_frame 写入；
                          调用方应在 win_frame 之后把 g_win_close 拷进来） */
    int _drag;         /* 内部：是否正在被拖动 */
    int _ox, _oy;      /* 内部：拖动抓取偏移 */
} Win;

#define WIN_MAX 8
extern Win  g_wins[WIN_MAX];
extern int  g_win_count;     /* 已注册的窗口数 */
extern int  g_win_focus;     /* 当前聚焦（最前）窗口的 id，-1 表示无 */

/* 注册一个窗口，返回槽位下标，容量满返回 -1。
 * 初始 open=1，z 取当前最大值+1（即注册即置顶）。 */
int  win_new(int x,int y,int w,int h,const char *title,int id);
/* 按 id 取窗口指针（找不到返回 NULL） */
Win *win_get(int id);
/* 关闭（隐藏）指定窗口 */
void win_close(int id);
/* 将指定窗口置顶（z 设为最大+1，并更新 g_win_focus） */
void win_bring_front(int id);
/* 每帧调用：处理所有窗口的标题栏拖动 + 点击置顶。
 * title_h 为可拖动标题栏高度（逻辑像素）。
 * *dragging 置 1 表示当前有窗口正在被拖动。
 * 返回当前聚焦（最前）窗口的 id。
 * 注意：标题栏右侧的关闭按钮区域（≈40px）不触发拖动。 */
int  win_update(int mx,int my,int ml,int title_h,int *dragging);
/* 按 z 从小到大绘制所有 open 窗口，对每个调用 draw_fn(win->id)。
 * 绘制顺序保证最前窗口最后画（覆盖其它）。 */
void win_draw_all(void (*draw_fn)(int id));

#endif /* HUDOS_WIN_H */
