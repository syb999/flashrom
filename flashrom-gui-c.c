/*
 * flashrom-gui-c.c —— CH341A SPI 编程器图形界面 (X11 + Xft, 纯 C, 中文正常)
 *
 * 分页界面: 备份 / 写入 / u-boot / 合成 / 日志   —— 每页只放本页功能, 不互相遮挡
 * 文本用 XftDrawStringUtf8 (fontconfig/Noto Sans CJK), 不用老式 8-bit X 字体
 * 执行: popen + poll, 与 X 连接一起等, 界面不卡
 *
 * 构建: gcc -O2 $(pkg-config --cflags xft) -o flashrom-gui-c flashrom-gui-c.c \
 *          -lXft -lXrender -lfontconfig -lX11
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#define FLASH_SIZE (16 * 1024 * 1024)     /* W25Q128 容量, 仅用于"合成16M" */
#define UBOOT_MIN  (16 * 1024)            /* u-boot 镜像合理范围 */
#define UBOOT_MAX  (1024 * 1024)

/* 以下全部在 init_paths() 里运行时推导 / 从配置文件读, 不写死 */
static char g_appdir[512];
static char g_flashrom[600];
static char g_tmpfile[600];
static char g_cfgfile[600];
static char g_fontui[128]  = "";
static char g_fontlog[128] = "";
#define MAXF       5
#define MAXBTN     16
#define MAXLINES   3000

enum { F_PROG, F_CHIP, F_BACKUP, F_IMAGE, F_UBOOT };

typedef struct { int x, y, w, h; const char *label; int action; } Btn;

static Display *dpy;
static int      scr;
static Window   win;
static GC       gc;
static Pixmap   pm;
static Visual  *vis;
static Colormap cmap;
static XftDraw *xd;
static XftFont *f_ui, *f_log;
static XftColor c_text, c_dim, c_ok, c_err;

static char fld[MAXF][512];
static int  focus = -1;
static int  page  = 0;                 /* 0备份 1写入 2uboot 3合成 4日志 */
static int  pkexec_on = 0;
static int  use_sudo  = 0;      /* 1 = 用 sudo 时间戳缓存(启动问一次, 之后不再问) */
static long long sudo_next = 0; /* 下次续期时间(ms) */
static char askpass[512];
static int  gui_lock_fd = -1;
static long g_chip_size;      /* 检测到的芯片容量(字节), 0=未知 */
static int  W = 720, H = 560;

static Btn  btns[MAXBTN];
static int  nbtn;

static char *lines[MAXLINES];
static int   nlines;
static long  log_off = -1;             /* -1 = 跟随底部 */

static FILE *pfp;
static int   running;
static int   last_rc;
/* 特权助手 (pkexec 只授权一次, helper 常驻) */
static int   helper_ok;
static int   helper_fd = -1;
static char  helper_dir[256];
static char  helper_out[300];
static char  g_action[80];       /* 当前动作名 */
static long  helper_pos;
static int   helper_busy;
static char  partial[2048];
static int   plen;

static int sel_a = -1, sel_b = -1, dragging;
static void redraw(void);
static const char *human_sz(long n, char *buf, size_t n2);
static void cut_line(char *ln);
static void layout(void);

/* ---------------- 文件选择器 ---------------- */
#include <dirent.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#define PICK_W 560
#define PICK_H 430
#define PICK_MAX 400

static Window   pick_win;
static Pixmap   pick_pm;
static XftDraw *pick_xd;
static int      pick_on;
static char     pick_dir[512];
static char     pick_name[PICK_MAX][256];
static int      pick_isdir[PICK_MAX];
static int      pick_n;
static int      pick_sel;
static int      pick_scroll;
static int      pick_field;
static int      pick_all;      /* 0=只显示镜像文件 */
static char     pick_edit[512];   /* 路径输入框内容 */
static int      pick_editing;     /* 是否正在输入路径 */
static int      pick_fresh;       /* 进入编辑后第一个字符替换原内容 */
static int         g_pct = -1;    /* 当前进度 0-100, -1=无 */
static long long pick_last_ms;    /* 双击判定 */
static int       pick_last_idx;

static void pick_draw(void);
static void pick_open(int field);
static void pick_click(int x, int y);
static void pick_close(void);
static void pick_load(const char *dir);


/* ---------------- 绘图小工具 ---------------- */
static int tw(const char *s, XftFont *f)
{
    XGlyphInfo e;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &e);
    return e.xOff;
}

static void tx(int x, int y, XftFont *f, XftColor *c, const char *s)
{
    XftDrawStringUtf8(xd, c, f, x, y + f->ascent, (const FcChar8 *)s, strlen(s));
}

static void box(int x, int y, int w, int h, unsigned long fill, unsigned long bd)
{
    XSetForeground(dpy, gc, fill);
    XFillRectangle(dpy, pm, gc, x, y, w, h);
    XSetForeground(dpy, gc, bd);
    XDrawRectangle(dpy, pm, gc, x, y, w - 1, h - 1);
}

/* 中文标题必须用 _NET_WM_NAME + UTF8_STRING, XStoreName 只支持 8-bit */
static void set_title_utf8(Window w, const char *title)
{
    Atom wmn = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom u8  = XInternAtom(dpy, "UTF8_STRING", False);
    int ascii = 1;
    for (const unsigned char *q = (const unsigned char *)title; *q; q++)
        if (*q >= 0x80) { ascii = 0; break; }
    if (ascii) XStoreName(dpy, w, title);      /* 仅 ASCII 才写 WM_NAME */
    if (wmn != None && u8 != None)
        XChangeProperty(dpy, w, wmn, u8, 8, PropModeReplace,
                        (const unsigned char *)title, strlen(title));
}

static void mkcolor(XftColor *c, const char *spec)
{
    if (!XftColorAllocName(dpy, vis, cmap, spec, c)) {
        XRenderColor rc = { 0, 0, 0, 0xFFFF };
        XftColorAllocValue(dpy, vis, cmap, &rc, c);
    }
}

/* ---------------- 日志 ---------------- */
static void log_add(const char *s)
{
    if (nlines == MAXLINES) {
        int drop = MAXLINES / 4;
        for (int i = 0; i < drop; i++) free(lines[i]);
        memmove(lines, lines + drop, (MAXLINES - drop) * sizeof(char *));
        nlines -= drop;
    }
    lines[nlines++] = strdup(s);
    log_off = -1;
}

static void log_fmt(const char *fmt, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_add(b);
}

/* 启动时弹一次密码(sudo -A + zenity), 之后靠 sudo 的时间戳缓存免密码;
 * 每 5 分钟 sudo -n -v 续期一次, 避免 15 分钟超时 */
static long long now_ms2(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sudo_prime(void)
{
    const char *home = getenv("HOME");
    FILE *f;

    if (!home) return;
    snprintf(askpass, sizeof askpass, "%s/.cache/flashrom_askpass.sh", home);
    f = fopen(askpass, "w");
    if (!f) return;
    {
        char self[512];
        ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
        if (sn <= 0) return;
        self[sn] = 0;
        fprintf(f, "#!/bin/sh\nexec '%s' --askpass\n", self);
    }
    fclose(f);
    chmod(askpass, 0700);

    {
        char cmd[700];
        snprintf(cmd, sizeof cmd,
                 "SUDO_ASKPASS=%s sudo -A -v >/dev/null 2>&1", askpass);
        use_sudo = (system(cmd) == 0);
    }
    sudo_next = now_ms2() + 5 * 60 * 1000;
}

static void sudo_keepalive(void)
{
    if (!use_sudo) return;
    if (now_ms2() < sudo_next) return;
    if (system("sudo -n -v >/dev/null 2>&1") != 0)
        sudo_prime();                    /* 缓存过期了再问一次 */
    sudo_next = now_ms2() + 5 * 60 * 1000;
}

static char g_helper_sh[512];

/* 启动特权助手: pkexec 只弹一次授权, 之后命令全部走它 */
static int helper_start(void)
{
    char cmdp[320];
    pid_t p;

    snprintf(helper_dir, sizeof helper_dir, "/tmp/.flashrom-gui.%d",
             (int)getpid());
    mkdir(helper_dir, 0700);
    snprintf(cmdp, sizeof cmdp, "%s/cmd", helper_dir);
    unlink(cmdp);
    if (mkfifo(cmdp, 0600) != 0) return 0;
    snprintf(helper_out, sizeof helper_out, "%s/out.txt", helper_dir);

    snprintf(g_helper_sh, sizeof g_helper_sh,
             "%s/flashrom-helper.sh", g_appdir);

    p = fork();
    if (p == 0) {
        execl("/usr/bin/pkexec", "pkexec", g_helper_sh, helper_dir,
              (char *)NULL);
        _exit(1);
    }
    /* 等 helper 打开管道(授权通过后才会), 最多 90 秒 */
    {
        int i;
        for (i = 0; i < 250; i++) {
            helper_fd = open(cmdp, O_WRONLY | O_NONBLOCK);
            if (helper_fd >= 0) return 1;
            usleep(100000);
        }
    }
    return 0;
}

static void helper_stop(void)
{
    if (helper_fd >= 0) {
        if (write(helper_fd, "__QUIT__\n", 9) < 0) { /* ignore */ }
        close(helper_fd);
        helper_fd = -1;
    }
}

/* 把命令交给助手, 返回后由 helper_poll() 读输出 */
static void helper_send(const char *cmd)
{
    char line[3072];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (write(helper_fd, line, strlen(line)) < 0) {
        log_fmt("✘ 助手通道写失败: %s", strerror(errno));
        return;
    }
    helper_pos = 0;
    helper_busy = 1;
    last_rc = 0;
}

/* 读助手输出; 完成返回 1 */
static int helper_poll(void)
{
    char buf[4096];
    int fd, n;
    long i;

    if (!helper_busy) return 0;
    fd = open(helper_out, O_RDONLY);
    if (fd < 0) return 0;
    if (lseek(fd, helper_pos, SEEK_SET) < 0) { close(fd); return 0; }
    while ((n = read(fd, buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                partial[plen] = 0;
                if (plen >= 8 && !strcmp(partial + plen - 8, "__DONE__")) {
                    plen = 0;
                    close(fd);
                    helper_busy = 0;
                    if (last_rc == 0)
                        log_fmt("✔ %s 完成", g_action[0] ? g_action : "操作");
                    else
                        log_fmt("✘ %s 失败", g_action[0] ? g_action : "操作");
                    redraw();
                    return 1;
                }
                cut_line(partial);
                plen = 0;
            } else if (plen < (int)sizeof partial - 1) {
                partial[plen++] = buf[i];
            }
        }
        helper_pos += n;
    }
    close(fd);
    redraw();
    return 0;
}

/* ---------------- 命令 ---------------- */
static void base_cmd(char *o, size_t n)
{
    snprintf(o, n, "%s -p %s", g_flashrom, fld[F_PROG]);
}
static void chip_arg(char *o, size_t n)
{
    o[0] = 0;
    if (!fld[F_CHIP][0]) return;      /* 空 = 交给 flashrom 自动探测 */
    snprintf(o, n, " -c %s", fld[F_CHIP]);
}

/* 把整条 shell 命令包成一个 pkexec, 保证一次操作只弹一次密码 */
static void wrap_pkexec(const char *cmd, char *out, size_t n)
{
    if (use_sudo) {
        /* 已有 sudo 缓存: 用 sudo -n, 不会再弹密码 */
        char esc2[3072];
        size_t j2 = 0;
        for (size_t i = 0; cmd[i] && j2 + 5 < sizeof esc2; i++) {
            if (cmd[i] == '\'') { memcpy(esc2 + j2, "'\\''", 4); j2 += 4; }
            else esc2[j2++] = cmd[i];
        }
        esc2[j2] = 0;
        snprintf(out, n, "sudo -n /bin/sh -c '%s' 2>&1", esc2);
        return;
    }
    if (!pkexec_on) { snprintf(out, n, "%s 2>&1", cmd); return; }
    /* 用单引号包住, 内部单引号转义成 '\'' */
    char esc[3072];
    size_t j = 0;
    for (size_t i = 0; cmd[i] && j + 5 < sizeof esc; i++) {
        if (cmd[i] == '\'') { memcpy(esc + j, "'\\''", 4); j += 4; }
        else esc[j++] = cmd[i];
    }
    esc[j] = 0;
    snprintf(out, n, "pkexec /bin/sh -c '%s' 2>&1", esc);
}

static void run_cmd(const char *cmd)
{
    char full[4096];
    if (running || !cmd || !*cmd) return;
    if (helper_ok) {                    /* 一次性授权后的常驻通道 */
        log_fmt("▶ %s", g_action[0] ? g_action : "执行中");
        running = 1;
        g_pct = -1;
        helper_send(cmd);
        return;
    }
    wrap_pkexec(cmd, full, sizeof full);
    log_fmt("▶ %s", g_action[0] ? g_action : "执行中");
    pfp = popen(full, "r");
    if (!pfp) { log_fmt("*** popen 失败 ***"); return; }
    fcntl(fileno(pfp), F_SETFL, O_NONBLOCK);
    running = 1; plen = 0;
    g_pct = -1;
    redraw();
}

static void do_detect(void)
{
    char b[600], cmd[3000];
    base_cmd(b, sizeof b);
    if (fld[F_CHIP][0]) {
        snprintf(cmd, sizeof cmd,
                 "%s -c %s --flash-name; echo ---; %s -c %s --flash-size",
                 b, fld[F_CHIP], b, fld[F_CHIP]);
    } else {
        /* 先不带 -c 通用探测; 失败再按候选型号逐个指定
         * (部分 CH341A 通用探测失败, 但指定型号能正常读) */
        snprintf(cmd, sizeof cmd,
            "FR='%s'; ok=''; "
            "for c in '' W25Q128.V W25Q64.V W25Q32.V W25Q16.V W25Q256.V "
            "W25Q80.V MX25L12835F MX25L64 MX25L32 SST25VF032B EN25Q128; do "
            "if [ -z \"$c\" ]; then A=''; else A=\"-c $c\"; fi; "
            "O=$($FR $A --flash-name 2>&1); "
            "case \"$O\" in *Found*flash*chip*) "
            "echo \"$O\"; echo ---; $FR $A --flash-size 2>&1 | tail -1; "
            "ok=1; break;; esac; done; "
            "[ -z \"$ok\" ] && echo 'No EEPROM/flash device found.'; true",
            b);
    }
    snprintf(g_action, sizeof g_action, "检测芯片");
    run_cmd(cmd);
}
static void do_backup(void)
{
    char b[600], ca[64], cmd[1400];
    base_cmd(b, sizeof b); chip_arg(ca, sizeof ca);
    snprintf(cmd, sizeof cmd, "%s%s -r \"%s\" && echo BACKUP_DONE",
             b, ca, fld[F_BACKUP]);
    snprintf(g_action, sizeof g_action, "备份整片");
    run_cmd(cmd);
}
static void do_write(void)
{
    char b[600], ca[64], cmd[1400];
    base_cmd(b, sizeof b); chip_arg(ca, sizeof ca);
    snprintf(cmd, sizeof cmd, "%s%s -w \"%s\"", b, ca, fld[F_IMAGE]);
    snprintf(g_action, sizeof g_action, "写入镜像");
    run_cmd(cmd);
}
static void do_verify(void)
{
    char b[600], ca[64], cmd[1400];
    base_cmd(b, sizeof b); chip_arg(ca, sizeof ca);
    snprintf(cmd, sizeof cmd, "%s%s -v \"%s\"", b, ca, fld[F_IMAGE]);
    snprintf(g_action, sizeof g_action, "校验镜像");
    run_cmd(cmd);
}
static void do_erase(void)
{
    char b[600], ca[64], cmd[1400];
    base_cmd(b, sizeof b); chip_arg(ca, sizeof ca);
    snprintf(cmd, sizeof cmd, "%s%s -E", b, ca);
    snprintf(g_action, sizeof g_action, "整片擦除");
    run_cmd(cmd);
}
/* 只写 u-boot 区: 读整片 -> 前 128KB 覆盖 -> 写回 (新版 flashrom 需整片镜像) */
static void do_uboot(void)
{
    char b[600], ca[64], cmd[3000];
    struct stat st;
    long blocks;

    if (stat(fld[F_UBOOT], &st) || !S_ISREG(st.st_mode)) {
        log_fmt("*** u-boot 镜像不存在: %s ***", fld[F_UBOOT]);
        redraw();
        return;
    }
    if (st.st_size < UBOOT_MIN || st.st_size > UBOOT_MAX) {
        log_fmt("*** u-boot 镜像大小异常 (%lld 字节), 应在 %d~%d 之间 ***",
                (long long)st.st_size, UBOOT_MIN, UBOOT_MAX);
        redraw();
        return;
    }
    blocks = ((long)st.st_size + 4095) / 4096;
    log_fmt("*** u-boot 镜像 %lld 字节 -> 只覆盖前 %ld 个 4K 块, 其余区域保持原样 ***",
            (long long)st.st_size, blocks);

    base_cmd(b, sizeof b); chip_arg(ca, sizeof ca);
    /* set -e: 任一步失败立即停止; 读回大小必须等于芯片容量, 否则绝不写 */
    snprintf(cmd, sizeof cmd,
        "set -e; "
        "F=\"%s\"; "
        "echo '--- 1/4 读取整片(保留原数据) ---'; "
        "%s%s -r \"$F\"; "
        "echo '--- 2/4 校验读取完整性 ---'; "
        "SZ=$(%s%s --flash-size | tail -1); "
        "ACT=$(stat -c %%s \"$F\"); "
        "if [ -z \"$SZ\" ] || [ \"$ACT\" != \"$SZ\" ]; then "
        "  echo \"*** 读取不完整(读到 $ACT, 芯片 $SZ), 已中止, 未写入任何数据 ***\"; "
        "  rm -f \"$F\"; exit 1; "
        "fi; "
        "echo \"--- 3/4 只替换前 %ld 个 4K 块 ---\"; "
        "dd if=\"%s\" of=\"$F\" bs=4096 count=%ld conv=notrunc; "
        "echo '--- 4/4 整体写回(其余区域为原数据) ---'; "
        "%s%s -w \"$F\"; "
        "rm -f \"$F\"; echo UBOOT_WRITE_DONE",
        g_tmpfile, b, ca, b, ca, blocks, fld[F_UBOOT], blocks, b, ca);
    snprintf(g_action, sizeof g_action, "写 u-boot 区");
    run_cmd(cmd);
}

static int build16m(void)
{
    FILE *f = fopen(fld[F_IMAGE], "wb");
    char buf[65536];
    long total = g_chip_size ? g_chip_size : FLASH_SIZE;   /* 按芯片容量 */
    size_t left = (size_t)total, n;
    if (!f) { log_fmt("*** 无法创建 %s ***", fld[F_IMAGE]); return -1; }
    {
        char hb[32];
        log_fmt("*** 目标镜像大小: %s ***", human_sz(total, hb, sizeof hb));
    }
    memset(buf, 0xFF, sizeof buf);
    while (left) {
        n = left > sizeof buf ? sizeof buf : left;
        fwrite(buf, 1, n, f);
        left -= n;
    }
    struct stat st;
    if (fld[F_UBOOT][0] && stat(fld[F_UBOOT], &st) == 0) {
        FILE *u = fopen(fld[F_UBOOT], "rb");
        if (u) {
            size_t pos = 0;
            fseeko(f, 0, SEEK_SET);
            while ((n = fread(buf, 1, sizeof buf, u)) > 0) {
                if (pos + n > (size_t)total) n = (size_t)total - pos;
                fwrite(buf, 1, n, f);
                pos += n;
                if (pos >= (size_t)total) break;
            }
            fclose(u);
            log_fmt("*** u-boot 写入镜像偏移 0 (%zu 字节) ***", pos);
        }
    }
    fclose(f);
    {
        char hb[32];
        log_fmt("*** 合成完成: %s (%s) ***", fld[F_IMAGE],
                human_sz(total, hb, sizeof hb));
    }
    return 0;
}

/* ---------------- 布局(分页) ---------------- */
static const char *PAGES[5] = { "备份", "写入", "u-boot", "合成", "日志" };

static void add_btn(int x, int y, int w, int h, const char *l, int a)
{
    if (nbtn >= MAXBTN) return;
    btns[nbtn].x = x; btns[nbtn].y = y; btns[nbtn].w = w; btns[nbtn].h = h;
    btns[nbtn].label = l; btns[nbtn].action = a; nbtn++;
}

static void layout(void)
{
    nbtn = 0;
    int bw = (W - 20) / 5;
    for (int i = 0; i < 5; i++)
        add_btn(10 + i * bw, 62, bw - 4, 28, PAGES[i], 100 + i);

    add_btn(W - 250, 32, 110, 26, "检测芯片", 1);
    add_btn(W - 372, 32, 116, 26, "编程器列表", 15);
    add_btn(W - 132, 32, 122, 26, pkexec_on ? "pkexec: 开" : "pkexec: 关", 11);

    switch (page) {
    case 0: add_btn(10, 134, 220, 32, "开始备份(读取整片)", 2);
            add_btn(W - 60, 100, 44, 24, "…", 20); break;
    case 1:
        add_btn(10, 134, 150, 32, "写入镜像", 3);
        add_btn(170, 134, 110, 32, "校验", 4);
        add_btn(290, 134, 130, 32, "整片擦除", 8);
        add_btn(W - 60, 100, 44, 24, "…", 21);
        break;
    case 2: add_btn(10, 134, 230, 32, "写 u-boot 区(保留其余)", 5);
            add_btn(W - 60, 100, 44, 24, "…", 22); break;
    case 3:
        add_btn(10, 134, 160, 32, "生成空白镜像", 6);
        add_btn(170, 134, 180, 32, "生成并写入(清空其余)", 7);
        add_btn(W - 60, 100, 44, 24, "…", 21);
        break;
    default:
        add_btn(10, H - 42, 110, 28, "清空日志", 10);
        add_btn(130, H - 42, 90, 28, "中止", 9);
        add_btn(230, H - 42, 110, 28, "复制选中", 12);
        add_btn(350, H - 42, 90, 28, "全选", 13);
        add_btn(450, H - 42, 110, 28, "复制全部", 14);
        break;
    }
}

/* ---------------- 绘制 ---------------- */
static void draw_field(int x, int y, int w, const char *label, const char *val,
                       int f)
{
    tx(x, y + 5, f_ui, &c_text, label);
    int fx = x + tw(label, f_ui) + 8;
    box(fx, y, w - (fx - x), 24, focus == f ? 0xFFFDE7 : 0xFFFFFF, 0x9AA0A6);
    if (val && val[0])
        tx(fx + 4, y + 5, f_ui, &c_text, val);
    else
        tx(fx + 4, y + 5, f_ui, &c_dim, "(自动检测)");   /* 未检测时的占位 */
}

static int log_area_y0, log_area_h, log_line_h, log_first;   /* redraw 时记下 */

static int log_hit_line(int wy)
{
    if (wy < log_area_y0 || wy >= log_area_y0 + log_area_h) return -1;
    int i = (wy - log_area_y0 - 5) / log_line_h;
    if (i < 0) return -1;
    int k = log_first + i;
    if (k < 0 || k >= nlines) return -1;
    return k;
}

static void copy_selection(void)
{
    int a = sel_a, b = sel_b;
    FILE *p;

    if (a < 0 || b < 0) { if (nlines == 0) return; a = 0; b = nlines - 1; }
    if (a > b) { int t = a; a = b; b = t; }
    p = popen("xclip -selection clipboard -i 2>/dev/null", "w");
    if (!p) { log_fmt("*** 复制失败: 没有 xclip ***"); return; }
    for (int i = a; i <= b && i < nlines; i++)
        fprintf(p, "%s\n", lines[i]);
    pclose(p);
    log_fmt("*** 已复制 %d 行到剪贴板 ***", b - a + 1);
    redraw();
}

/* 画一行, 超出宽度自动折行; 返回占用的行数 */
static int draw_wrap(int x, int y0, XftFont *f, XftColor *c, const char *s,
                     int maxw, int per)
{
    char buf[4096];
    int rows = 0, y = y0;

    while (*s && rows < 200) {
        size_t len = strlen(s), take = len;
        snprintf(buf, sizeof buf, "%s", s);
        if (tw(buf, f) > maxw) {
            take = (size_t)maxw / 7;            /* 估算: 约 7px/字符 */
            if (take < 1) take = 1;
            if (take > len) take = len;
            while (take > 1) {
                buf[take] = 0;
                if (tw(buf, f) <= maxw) break;
                take--;
            }
            buf[take] = 0;
            while (take > 1 && ((unsigned char)s[take] & 0xC0) == 0x80) take--;
            buf[take] = 0;
        }
        tx(x, y, f, c, buf);
        y += per;
        rows++;
        s += take;
    }
    return rows ? rows : 1;
}

static void redraw(void)
{
    XWindowAttributes wa;
    int per, fit, first, yy;

    if (XGetWindowAttributes(dpy, win, &wa) &&
        (wa.width != W || wa.height != H)) {
        W = wa.width; H = wa.height;
        if (pm) XFreePixmap(dpy, pm);
        pm = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
        if (xd) XftDrawDestroy(xd);
        xd = XftDrawCreate(dpy, pm, vis, cmap);
        layout();
    }

    XSetForeground(dpy, gc, 0xF2F2F2);
    XFillRectangle(dpy, pm, gc, 0, 0, W, H);

    tx(10, 6, f_ui, &c_text, "Flashrom 编程器 · CH341A");

    /* 顶部固定行: 编程器 / 芯片 —— 宽度按右侧按钮的实际位置动态计算, 避免重叠 */
    {
        int bx_list = W - 372;          /* 「编程器列表」按钮左边 */
        int f1_x = 10, f1_w = 176;      /* 编程器: 10 .. 186 */
        int f2_x = f1_x + f1_w + 8;     /* 芯片从 194 开始 */
        int f2_w = bx_list - f2_x - 8;  /* 到按钮左边为止 */
        if (f2_w < 100) {               /* 窗口太窄就把编程器框收一点 */
            f1_w -= (100 - f2_w);
            if (f1_w < 90) f1_w = 90;
            f2_x = f1_x + f1_w + 8;
            f2_w = bx_list - f2_x - 8;
        }
        draw_field(f1_x, 32, f1_w, "编程器", fld[F_PROG], F_PROG);
        draw_field(f2_x, 32, f2_w, "芯片", fld[F_CHIP], F_CHIP);
    }

    /* 标签栏 */
    for (int i = 0; i < nbtn; i++) {
        Btn *b = &btns[i];
        if (b->action < 100) continue;
        box(b->x, b->y, b->w, b->h,
            (b->action - 100 == page) ? 0xD0E0F8 : 0xE4E7EA, 0x9AA0A6);
        tx(b->x + (b->w - tw(b->label, f_ui)) / 2,
           b->y + (b->h - f_ui->ascent - f_ui->descent) / 2, f_ui, &c_text,
           b->label);
    }

    /* 各页内容 */
    if (page == 0) {
        draw_field(10, 100, W - 20, "备份到:", fld[F_BACKUP], F_BACKUP);
        {
            char hb[32], t[160];
            human_sz(g_chip_size, hb, sizeof hb);
            snprintf(t, sizeof t,
                     "读取整片(%s)存为上面的文件; 完成后日志显示 md5。",
                     g_chip_size ? hb : "点“检测芯片”获取容量");
            tx(10, 176, f_ui, &c_dim, t);
        }
    } else if (page == 1) {
        draw_field(10, 100, W - 20, "镜像文件:", fld[F_IMAGE], F_IMAGE);
        tx(10, 176, f_ui, &c_dim, "写入=整片烧录; 校验=回读比对; 擦除=清空整片。");
    } else if (page == 2) {
        draw_field(10, 100, W - 20, "u-boot:", fld[F_UBOOT], F_UBOOT);
        tx(10, 176, f_ui, &c_dim,
           "按文件实际大小写入: 64K/128K/192K/256K 均可 (16K~1M)");
        {
            char uinfo[160];
            struct stat ust;
            if (fld[F_UBOOT][0] && stat(fld[F_UBOOT], &ust) == 0)
                snprintf(uinfo, sizeof uinfo,
                         "当前镜像: %lld 字节 (%lld K) -- 只替换这一段",
                         (long long)ust.st_size, (long long)ust.st_size / 1024);
            else
                snprintf(uinfo, sizeof uinfo, "未选择 u-boot 镜像");
            tx(10, 196, f_ui, &c_dim, uinfo);
        }
        tx(10, 216, f_ui, &c_dim,
           "流程: 读整片 -> 校验读全 -> 只改前面 -> 整体写回");
    } else if (page == 3) {
        draw_field(10, 100, W - 20, "输出镜像:", fld[F_IMAGE], F_IMAGE);
        {
            char hb[32], t[200];
            human_sz(g_chip_size ? g_chip_size : FLASH_SIZE, hb, sizeof hb);
            snprintf(t, sizeof t,
                     "生成 %s 全 0xFF + u-boot@0 的空白镜像%s。", hb,
                     g_chip_size ? "(按检测到的芯片容量)"
                                 : "(未检测, 默认16MB)");
            tx(10, 176, f_ui, &c_dim, t);
        }
        tx(10, 196, f_ui, &c_dim,
           "注意: 用它写入会清空芯片其余区域(适合全空片), 不是只写 u-boot!");
    }

    /* 日志区 */
    int ly = (page == 4) ? 96 : (page == 2 ? 252 : 226);
    int lh = H - ly - ((page == 4) ? 52 : 34);
    if (lh < 50) lh = 50;
    box(10, ly, W - 20, lh, 0xFFFFFF, 0x9AA0A6);

    per = f_log->ascent + f_log->descent + 3;
    log_line_h = per;
    log_area_y0 = ly;
    log_area_h = lh;
    fit = (lh - 8) / per;
    first = 0;
    if (nlines > fit) {
        first = (log_off < 0) ? (nlines - fit) : (int)log_off;
        if (first > nlines - fit) first = nlines - fit;
        if (first < 0) first = 0;
    }
    log_first = first;
    yy = ly + 5;
    for (int i = first; i < nlines && yy < ly + lh - per; i++) {
        XftColor *c = &c_text;
        if (strstr(lines[i], "rror")) c = &c_err;
        else if (strstr(lines[i], "DONE") || strstr(lines[i], "VERIFIED") ||
                 strstr(lines[i], "Success")) c = &c_ok;
        if (sel_a >= 0 && sel_b >= 0) {
            int lo = sel_a < sel_b ? sel_a : sel_b;
            int hi = sel_a < sel_b ? sel_b : sel_a;
            if (i >= lo && i <= hi) {
                XSetForeground(dpy, gc, 0xCDE4FF);
                XFillRectangle(dpy, pm, gc, 14, yy - 1, W - 28, per);
            }
        }
        {
            int rows = draw_wrap(16, yy, f_log, c, lines[i], W - 76, per);
            yy += per * rows;
        }
    }

    /* 页内按钮 */
    for (int i = 0; i < nbtn; i++) {
        Btn *b = &btns[i];
        if (b->action >= 100) continue;
        box(b->x, b->y, b->w, b->h, 0xE4E7EA, 0x9AA0A6);
        tx(b->x + (b->w - tw(b->label, f_ui)) / 2,
           b->y + (b->h - f_ui->ascent - f_ui->descent) / 2, f_ui, &c_text,
           b->label);
    }

    char st[200];
    snprintf(st, sizeof st, "%s   |   %d 行日志",
             running ? "运行中…" : "就绪", nlines);
    tx(10, H - 20, f_ui, &c_dim, st);

    XCopyArea(dpy, pm, win, gc, 0, 0, W, H, 0, 0);
    XFlush(dpy);
}

/* 解析 flashrom --progress 的 "[write: 45%]" 之类输出 */
static int parse_progress(const char *ln, int *out)
{
    const char *pc = strchr(ln, '%');
    const char *br = strchr(ln, '[');
    const char *co = strchr(ln, ':');
    if (!pc || !br || !co || br > co || co > pc) return 0;
    const char *q = co + 1;
    while (*q == ' ') q++;
    if (*q < '0' || *q > '9') return 0;
    { int v = 0;
      while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
      if (v < 0 || v > 100) return 0;
      *out = v;
      return 1;
    }
}

/* ---------------- 子进程输出 ---------------- */
/* 从 flashrom 输出里抓芯片名:
 *   Found Winbond flash chip "W25Q128.V" (16384 kB, SPI) on ch341a_spi.
 *   vendor="Winbond" name="W25Q128.V"
 */
static const char *human_sz(long n, char *buf, size_t n2)
{
    if (n <= 0) { snprintf(buf, n2, "未知大小"); return buf; }
    if (n >= (1L << 20)) snprintf(buf, n2, "%ld MB", n >> 20);
    else if (n >= 1024)  snprintf(buf, n2, "%ld KB", n >> 10);
    else                 snprintf(buf, n2, "%ld B", n);
    return buf;
}

/* 抓容量: "(16384 kB," 或 纯数字行 */
static void try_chip_size(const char *ln)
{
    const char *p = strstr(ln, " kB,");
    if (p) {
        const char *q = p;
        while (q > ln && (q[-1] >= '0' && q[-1] <= '9')) q--;
        if (q < p) {
            long v = atol(q);
            if (v >= 32 && v <= 512 * 1024) { g_chip_size = v * 1024; return; }
        }
    }
    {   /* 纯数字行: flashrom --flash-size 最后一行 */
        const char *t = ln;
        while (*t == ' ') t++;
        int alld = 1, len = 0;
        for (const char *u = t; *u && *u != '\n' && *u != '\r'; u++, len++)
            if (*u < '0' || *u > '9') alld = 0;
        if (alld && len >= 5) {
            long v = atol(t);
            if (v >= 128L * 1024 && v <= 512L * 1024 * 1024)
                g_chip_size = v;
        }
    }
}

static void try_chip_name(const char *ln)
{
    const char *p = NULL;

    if ((p = strstr(ln, "flash chip \"")) != NULL) p += 12;
    else if ((p = strstr(ln, "name=\"")) != NULL) p += 6;
    if (!p) return;

    char nm[64];
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof nm - 1) nm[i++] = *p++;
    nm[i] = 0;
    if (!nm[0]) return;
    if (strcmp(nm, fld[F_CHIP]) == 0) return;

    snprintf(fld[F_CHIP], sizeof fld[0], "%s", nm);
    log_fmt("*** 检测到芯片: %s ***", nm);
    layout();
    redraw();
}

static void cut_line(char *ln)
{
    static int banner_seen, free_seen, src_seen;
    int v;

    /* 每次调用 flashrom 都会重复打这三行横幅, 只保留第一次 */
    if (strstr(ln, "flashrom v")) {
        if (banner_seen) return;
        banner_seen = 1;
    } else if (strstr(ln, "flashrom is free software")) {
        if (free_seen) return;
        free_seen = 1;
    } else if (strstr(ln, "get the source code at")) {
        if (src_seen) return;
        src_seen = 1;
    }
    if (parse_progress(ln, &v)) {          /* [write: 45%] -> 只更新进度 */
        g_pct = v;
        redraw();
        return;
    }
    if (strstr(ln, "Progress")) return;
    if (ln[0] == '$' ) return;                      /* 命令回显 */
    if (strstr(ln, "No EEPROM/flash device found")) {
        static int hinted;
        if (!hinted) {
            hinted = 1;
            log_add("*** 未检测到 SPI 芯片, 请依次检查: ***");
            log_add("  1) 夹子/插座 8 个脚是否全部接触良好(最常见)");
            log_add("  2) CH341A 上的电平跳线必须选 3.3V (W25Q 系列是 3.3V)");
            log_add("  3) 芯片 1 脚方向与插座缺口方向是否一致");
            log_add("  4) 芯片是否已被焊在板子上(在线刷需断开主板供电)");
            log_add("  5) 若芯片是 1.8V 型号, 需 1.8V 转接板");
            log_add("  (详细过程: 命令行跑 flashrom -p ch341a_spi -V 看 RDID 返回)");
        }
        return;                                      /* 原始英文行不再重复刷 */
    }
    if (strstr(ln, "flashrom is free software")) return;
    if (strstr(ln, "get the source code at")) return;
    if (strstr(ln, "flashrom v")) {
        static int vseen;
        if (vseen) return;
        vseen = 1;
    }
    try_chip_name(ln);          /* 有芯片名就回填输入框 */
    try_chip_size(ln);          /* 记下芯片容量 */
    log_add(ln);
}

static void pump_child(void)
{
    char buf[4096];
    ssize_t n;

    if (!pfp) return;
    n = read(fileno(pfp), buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (plen) { partial[plen] = 0; cut_line(partial); plen = 0; }
            } else if (plen < (int)sizeof partial - 1) {
                partial[plen++] = buf[i];
            }
        }
        redraw();
        return;
    }
    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        int st = 0;
        if (plen) { partial[plen] = 0; cut_line(partial); plen = 0; }
        st = pclose(pfp);
        last_rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        pfp = NULL; running = 0;
        g_pct = -1;
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);   /* 从 pkexec 弹窗抢回焦点 */
        if (last_rc == 0)
            log_fmt("✔ %s 完成", g_action[0] ? g_action : "操作");
        else
            log_fmt("✘ %s 失败 (返回码 %d)", g_action[0] ? g_action : "操作", last_rc);
        redraw();
    }
}

static void stop_child(void)
{
    if (!pfp) return;
    char k[256];
    snprintf(k, sizeof k, "pkill -f '%s -p' 2>/dev/null", g_flashrom);
    if (system(k) == -1) { /* ignore */ }
    pclose(pfp);
    pfp = NULL; running = 0;
    log_fmt("*** 已中止 ***");
    redraw();
}

/* ---------------- 交互 ---------------- */
static void on_click(int x, int y)
{
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);   /* 点击即拿回焦点 */
    for (int i = 0; i < nbtn; i++) {
        Btn *b = &btns[i];
        if (!(x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h))
            continue;
        if (b->action >= 100) {
            page = b->action - 100;
            focus = -1; log_off = -1;
            layout(); redraw(); return;
        }
        switch (b->action) {
        case 1:  do_detect(); break;
        case 2:  do_backup(); break;
        case 3:  do_write();  break;
        case 4:  do_verify(); break;
        case 5:  do_uboot();  break;
        case 6:  build16m(); redraw(); break;
        case 7:  if (build16m() == 0) do_write(); redraw(); break;
        case 8:  do_erase();  break;
        case 9:  stop_child(); break;
        case 10: for (int k = 0; k < nlines; k++) free(lines[k]);
                 nlines = 0; log_off = -1; redraw(); break;
        case 11: pkexec_on = !pkexec_on; layout(); redraw(); break;
        case 12: copy_selection(); break;
        case 13: sel_a = 0; sel_b = nlines ? nlines - 1 : 0; redraw(); break;
        case 14: sel_a = -1; sel_b = -1; copy_selection(); break;
        case 15: {
            char c2[600];
            snprintf(c2, sizeof c2,
                "%s --list-supported 2>&1 | "
                "sed -n \"/Supported programmers/,+4p\"", g_flashrom);
            run_cmd(c2);
            log_fmt("提示: 编程器可填 ch341a_spi / ch347_spi / "
                    "serprog:dev=/dev/ttyUSB0:2000000 / "
                    "linux_spi:dev=/dev/spidev0.0 等");
            redraw();
            break;
        }
        case 20: pick_open(F_BACKUP); break;
        case 21: pick_open(F_IMAGE);  break;
        case 22: pick_open(F_UBOOT);  break;
        }
        return;
    }

    /* 输入框聚焦 */
    focus = -1;
    if (y >= 32 && y < 58) {
        int bx_list = W - 372;
        int f2_x = 10 + 176 + 8;
        if (x < f2_x) focus = F_PROG;
        else if (x < bx_list) focus = F_CHIP;
    } else if (y >= 100 && y < 126 && x > 60) {
        if (page == 0) focus = F_BACKUP;
        else if (page == 1 || page == 3) focus = F_IMAGE;
        else if (page == 2) focus = F_UBOOT;
    }
    redraw();
}

static void on_key(XKeyEvent *e)
{
    char buf[32];
    KeySym ks;
    int n = XLookupString(e, buf, sizeof buf, &ks, NULL);

    if (ks == XK_Escape) { focus = -1; redraw(); return; }
    if (focus < 0) return;
    if (ks == XK_BackSpace) {
        size_t l = strlen(fld[focus]);
        if (l) {
            l--;
            while (l && ((unsigned char)fld[focus][l] & 0xC0) == 0x80) l--;
            fld[focus][l] = 0;
        }
        redraw(); return;
    }
    if (ks == XK_Return) { focus = -1; redraw(); return; }
    if (n > 0) {
        size_t l = strlen(fld[focus]);
        if (l + (size_t)n < sizeof fld[0] - 1) {
            memcpy(fld[focus] + l, buf, n);
            fld[focus][l + n] = 0;
            redraw();
        }
    }
}


/* ---------------- 文件选择器实现 ---------------- */
static int cmp_entry(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    /* ".." 永远第一; 然后文件在前(要选的是文件), 目录在后; 同类按名字 */
    if (!strcmp(pick_name[ia], "..")) return -1;
    if (!strcmp(pick_name[ib], "..")) return 1;
    if (pick_isdir[ia] != pick_isdir[ib]) return pick_isdir[ia] - pick_isdir[ib];
    return strcmp(pick_name[ia], pick_name[ib]);
}

static void pick_load(const char *dir)
{
    DIR *d;
    struct dirent *e;
    static int idx[PICK_MAX];

    /* 注意: 调用方可能直接把 pick_dir 传进来, snprintf 自覆盖是 UB —— 先拷到临时缓冲 */
    {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", dir ? dir : "");
        snprintf(pick_dir, sizeof pick_dir, "%s", tmp);
    }
    pick_n = 0;
    pick_sel = 0;
    pick_scroll = 0;

    strcpy(pick_name[0], "..");
    pick_isdir[0] = 1;
    pick_n = 1;

    d = opendir(pick_dir);
    if (!d) {
        strcpy(pick_name[1], "(目录打不开)");
        pick_isdir[1] = 0;
        pick_n = 2;
        return;
    }
    if (d) {
        while ((e = readdir(d)) && pick_n < PICK_MAX - 1) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[1024];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            if (stat(full, &st)) continue;
            if (S_ISDIR(st.st_mode)) {
                snprintf(pick_name[pick_n], 256, "%s", e->d_name);
                pick_isdir[pick_n] = 1;
                pick_n++;
            } else if (S_ISREG(st.st_mode)) {
                const char *nm = e->d_name;
                const char *dot = strrchr(nm, '.');
                int want = pick_all;
                if (!want && dot) {
                    if (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".img") ||
                        !strcasecmp(dot, ".rom") || !strcasecmp(dot, ".BIN") ||
                        !strcasecmp(dot, ".hex") || !strcasecmp(dot, ".fw"))
                        want = 1;
                }
                if (!want) continue;
                snprintf(pick_name[pick_n], 256, "%s", nm);
                pick_isdir[pick_n] = 0;
                pick_n++;
            }
        }
        closedir(d);
    }
    for (int i = 0; i < pick_n; i++) idx[i] = i;
    qsort(idx, pick_n, sizeof(int), cmp_entry);
    /* 重新排列 */
    static char nm[PICK_MAX][256];
    static int  dd[PICK_MAX];
    for (int i = 0; i < pick_n; i++) {
        memcpy(nm[i], pick_name[idx[i]], 256);
        dd[i] = pick_isdir[idx[i]];
    }
    for (int i = 0; i < pick_n; i++) {
        memcpy(pick_name[i], nm[i], 256);
        pick_isdir[i] = dd[i];
    }
    if (!pick_editing) snprintf(pick_edit, sizeof pick_edit, "%s", pick_dir);
}

static void pick_draw(void)
{
    XftDraw *save = xd;
    if (!pick_on) return;
    if (!pick_xd) pick_xd = XftDrawCreate(dpy, pick_pm, vis, cmap);
    xd = pick_xd;
    XSetForeground(dpy, gc, 0xF2F2F2);
    XFillRectangle(dpy, pick_pm, gc, 0, 0, PICK_W, PICK_H);

    tx(10, 8, f_ui, &c_text, "选择文件");
    /* 路径输入框: 可直接输入任意路径, 回车跳转 */
    box(10, 34, PICK_W - 20, 24, pick_editing ? 0xFFFDE7 : 0xFFFFFF,
        pick_editing ? 0x4A80D0 : 0x9AA0A6);
    {
        char show[560];
        snprintf(show, sizeof show, "%s%s", pick_edit,
                 pick_editing ? "_" : "");
        tx(14, 39, f_ui, pick_editing ? &c_text : &c_dim, show);
    }

    /* 列表 */
    int ly = 64, lh = PICK_H - 64 - 46;
    box(10, ly, PICK_W - 20, lh, 0xFFFFFF, 0x9AA0A6);
    int per = f_ui->ascent + f_ui->descent + 6;
    int fit = (lh - 6) / per;
    if (pick_sel < pick_scroll) pick_scroll = pick_sel;
    if (pick_sel >= pick_scroll + fit) pick_scroll = pick_sel - fit + 1;
    for (int i = 0; i < fit; i++) {
        int k = pick_scroll + i;
        if (k >= pick_n) break;
        int yy = ly + 4 + i * per;
        if (k == pick_sel) {
            XSetForeground(dpy, gc, 0xD0E0F8);
            XFillRectangle(dpy, pick_pm, gc, 12, yy - 2, PICK_W - 24, per);
        }
        char line[300];
        snprintf(line, sizeof line, "%s%s",
                 pick_isdir[k] ? "[目录] " : "       ", pick_name[k]);
        tx(18, yy, f_ui, &c_text, line);
    }

    /* 右侧滚动条 + 项数 */
    if (pick_n > fit) {
        int track = lh - 8;
        int bar = track * fit / pick_n;
        if (bar < 20) bar = 20;
        int maxs = pick_n - fit;
        int by = ly + 4 + (track - bar) * pick_scroll / (maxs > 0 ? maxs : 1);
        XSetForeground(dpy, gc, 0xB0B6BC);
        XFillRectangle(dpy, pick_pm, gc, PICK_W - 20, by, 8, bar);
    }
    {
        char cnt[128];
        snprintf(cnt, sizeof cnt, "共 %d 项%s", pick_n,
                 pick_all ? "" : "（仅镜像文件）");
        tx(PICK_W - 160, 8, f_ui, &c_dim, cnt);
        tx(120, 8, f_ui, &c_dim, "点路径框可直接输入");
    }

    /* 按钮 */
    box(10, PICK_H - 38, 90, 28, 0xE4E7EA, 0x9AA0A6);
    tx(10 + (90 - tw("打开", f_ui)) / 2, PICK_H - 38 + 6, f_ui, &c_text, "打开");
    box(110, PICK_H - 38, 100, 28, 0xE4E7EA, 0x9AA0A6);
    tx(110 + (100 - tw("上级目录", f_ui)) / 2, PICK_H - 38 + 6, f_ui, &c_text, "上级目录");
    box(220, PICK_H - 38, 130, 28, 0xE4E7EA, 0x9AA0A6);
    tx(220 + (130 - tw(pick_all ? "显示: 全部" : "显示: 仅镜像", f_ui)) / 2,
       PICK_H - 38 + 6, f_ui, &c_text, pick_all ? "显示: 全部" : "显示: 仅镜像");
    box(PICK_W - 120, PICK_H - 38, 110, 28, 0xE4E7EA, 0x9AA0A6);
    tx(PICK_W - 120 + (110 - tw("取消", f_ui)) / 2, PICK_H - 38 + 6, f_ui, &c_text, "取消");

    xd = save;
    XCopyArea(dpy, pick_pm, pick_win, gc, 0, 0, PICK_W, PICK_H, 0, 0);
    XFlush(dpy);
}

static void pick_close(void)
{
    pick_on = 0;
    XUnmapWindow(dpy, pick_win);
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    XFlush(dpy);
    redraw();
}

static void pick_open(int field)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", fld[field]);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (!dir[0]) snprintf(dir, sizeof dir, "%s", g_appdir);
    pick_field = field;
    pick_load(dir);
    snprintf(pick_edit, sizeof pick_edit, "%s", pick_dir);
    pick_editing = 0;
    pick_on = 1;
    XMapRaised(dpy, pick_win);
    XRaiseWindow(dpy, pick_win);
    XSync(dpy, False);          /* 等 map 生效, 不要对未 viewable 的窗口 SetInputFocus */
    pick_draw();
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 打开当前选中项: 目录->进入, 文件->采用并关闭 */
static void pick_activate(void)
{
    char nd[1024];

    if (pick_n == 0) return;
    if (pick_isdir[pick_sel]) {
        if (!strcmp(pick_name[pick_sel], "..")) {
            char *sl = strrchr(pick_dir, '/');
            if (sl && sl != pick_dir) *sl = 0;
            else snprintf(pick_dir, sizeof pick_dir, "/");
            snprintf(nd, sizeof nd, "%s", pick_dir);
        } else {
            snprintf(nd, sizeof nd, "%s/%s", pick_dir, pick_name[pick_sel]);
        }
        pick_load(nd);
        pick_draw();
    } else {
        snprintf(fld[pick_field], sizeof fld[0], "%s/%s", pick_dir,
                 pick_name[pick_sel]);
        pick_close();
    }
}

static void pick_click(int x, int y)
{
    int per = f_ui->ascent + f_ui->descent + 6;
    int ly = 64, lh = PICK_H - 64 - 46;
    int fit = (lh - 6) / per;

    if (y >= 34 && y < 58) {                 /* 点路径框 = 编辑 */
        pick_editing = 1;
        pick_fresh = 1;
        pick_draw();
        return;
    }
    if (y >= PICK_H - 40 && y < PICK_H - 6) {
        if (x >= 10 && x < 100) { pick_activate(); return; }
        if (x >= 220 && x < 350) {                      /* 过滤开关 */
            pick_all = !pick_all;
            pick_load(pick_dir);
            pick_draw();
            return;
        }
        if (x >= 110 && x < 210) {                      /* 上级 */
            char *sl = strrchr(pick_dir, '/');
            if (sl && sl != pick_dir) *sl = 0;
            else snprintf(pick_dir, sizeof pick_dir, "/");
            pick_load(pick_dir);
            pick_draw();
            return;
        }
        if (x >= PICK_W - 120) { pick_close(); return; }
        return;
    }

    /* 列表点击 */
    if (y >= ly && y < ly + lh && pick_n) {
        int i = (y - ly - 4) / per;
        int k = pick_scroll + i;
        if (i >= 0 && i < fit && k < pick_n) {
            long long t = now_ms();
            if (k == pick_last_idx && t - pick_last_ms < 450) {   /* 双击 */
                pick_sel = k;
                pick_activate();
                pick_last_idx = -1;
                return;
            }
            pick_last_idx = k;
            pick_last_ms = t;
            pick_sel = k;
            pick_draw();
        }
    }
}

/* ---------------- 内置密码输入框 (SUDO_ASKPASS) ----------------
 * 不依赖 zenity/kdialog: sudo 会执行 "<本程序> --askpass",
 * 我们自己弹一个 X11 小窗口读密码, 输完打印到 stdout 并退出.
 */
static Window ap_win_g;
static XftDraw *ap_xd;
static XftFont *ap_font;
static XftColor ap_fg, ap_bg;
static char ap_pw[128];
static int  ap_done, ap_ok;

static void ap_paint(void)
{
    int W2 = 360, H2 = 132;
    XSetForeground(dpy, gc, 0xFFFFFF);
    XFillRectangle(dpy, ap_win_g, gc, 0, 0, W2, H2);
    XSetForeground(dpy, gc, 0x9AA0A6);
    XDrawRectangle(dpy, ap_win_g, gc, 0, 0, W2 - 1, H2 - 1);
    tx(14, 10, ap_font, &ap_fg, "需要管理员权限, 请输入登录密码:");
    /* 密码框(掩码) */
    XSetForeground(dpy, gc, 0xF2F2F2);
    XFillRectangle(dpy, ap_win_g, gc, 14, 38, W2 - 28, 26);
    XSetForeground(dpy, gc, 0x9AA0A6);
    XDrawRectangle(dpy, ap_win_g, gc, 14, 38, W2 - 29, 25);
    {
        char mask[140];
        size_t n = strlen(ap_pw), i;
        if (n > 40) n = 40;
        for (i = 0; i < n; i++) mask[i] = '*';
        mask[n] = 0;
        tx(20, 44, ap_font, &ap_fg, mask);
    }
    /* 按钮 */
    XSetForeground(dpy, gc, 0x2E7D32);
    XFillRectangle(dpy, ap_win_g, gc, 150, 90, 90, 28);
    tx(150 + 30, 96, ap_font, &ap_bg, "确定");
    XSetForeground(dpy, gc, 0xE4E7EA);
    XFillRectangle(dpy, ap_win_g, gc, 252, 90, 90, 28);
    XSetForeground(dpy, gc, 0x9AA0A6);
    XDrawRectangle(dpy, ap_win_g, gc, 252, 90, 89, 27);
    tx(252 + 30, 96, ap_font, &ap_fg, "取消");
    XFlush(dpy);
}

static int askpass_main(void)
{
    Window w;
    int scr2;
    Visual *vis2;
    Colormap cmap2;
    XWindowAttributes wa;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    scr2 = DefaultScreen(dpy);
    vis2 = DefaultVisual(dpy, scr2);
    cmap2 = DefaultColormap(dpy, scr2);
    vis = vis2; cmap = cmap2;   /* mkcolor()/tx() 用的全局 */
    scr = scr2;
    gc = XCreateGC(dpy, RootWindow(dpy, scr2), 0, NULL);

    w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr2), 420, 300, 360, 132,
                            0, 0xFFFFFF, 0xFFFFFF);
    ap_win_g = w;
    set_title_utf8(w, "Flashrom 需要管理员权限");
    XSelectInput(dpy, w, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapRaised(dpy, w);
    XStoreName(dpy, w, "Flashrom askpass");

    ap_xd = XftDrawCreate(dpy, w, vis2, cmap2);
    xd = ap_xd;                 /* tx() 画到 askpass 窗口 */
    ap_font = XftFontOpenName(dpy, scr2, "Noto Sans CJK SC-11");
    if (!ap_font) ap_font = XftFontOpenName(dpy, scr2, "Sans-11");
    mkcolor(&ap_fg, "#202124");
    mkcolor(&ap_bg, "#ffffff");

    while (!ap_done) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose) {
            ap_paint();
        } else if (e.type == KeyPress) {
            char b[16];
            KeySym ks;
            int n = XLookupString(&e.xkey, b, sizeof b, &ks, NULL);
            if (ks == XK_Return) { ap_done = 1; ap_ok = 1; }
            else if (ks == XK_Escape) { ap_done = 1; }
            else if (ks == XK_BackSpace) {
                size_t l = strlen(ap_pw);
                if (l) ap_pw[l - 1] = 0;
            } else if (n > 0 && strlen(ap_pw) + (size_t)n < sizeof ap_pw - 1) {
                strncat(ap_pw, b, (size_t)n);
            }
            ap_paint();
        } else if (e.type == ButtonPress) {
            int x = e.xbutton.x, y = e.xbutton.y;
            if (y >= 90 && y <= 118) {
                if (x >= 150 && x <= 240) { ap_done = 1; ap_ok = 1; }
                else if (x >= 252 && x <= 342) { ap_done = 1; }
            }
        }
    }
    (void)wa;
    if (ap_ok && ap_pw[0]) {
        printf("%s\n", ap_pw);
        fflush(stdout);
        return 0;
    }
    return 1;
}

/* ---------------- 路径 / 配置 ---------------- */
static void cfg_path(char *o, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(o, n, "%s/flashrom-gui.conf", xdg);
    else if (home && *home) snprintf(o, n, "%s/.config/flashrom-gui.conf", home);
    else snprintf(o, n, "./flashrom-gui.conf");
}

static void init_paths(void)
{
    char exe[512] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *sl = strrchr(exe, '/');
        if (sl) { *sl = 0; snprintf(g_appdir, sizeof g_appdir, "%s", exe); }
    }
    if (!g_appdir[0]) snprintf(g_appdir, sizeof g_appdir, ".");

    const char *fb = getenv("FLASHROM_BIN");
    if (fb && *fb) snprintf(g_flashrom, sizeof g_flashrom, "%s", fb);
    else snprintf(g_flashrom, sizeof g_flashrom, "%s/flashrom", g_appdir);

    const char *td = getenv("TMPDIR");
    if (!td || !*td) td = "/tmp";
    snprintf(g_tmpfile, sizeof g_tmpfile, "%s/flashrom_full_%d.bin", td,
             (int)getpid());

    cfg_path(g_cfgfile, sizeof g_cfgfile);
}

/* 检查 CH341A (1a86:5512) 的 /dev/bus/usb 节点是否当前用户可写;
 * 不可写说明没装 udev 规则 -> 自动打开 pkexec, 否则 flashrom 连设备都打不开 */
static int ch341a_accessible(void)
{
    const char *base = "/sys/bus/usb/devices";
    DIR *d = opendir(base);
    struct dirent *e;
    int ok = 0;

    if (!d) return 1;                       /* 判断不了就当可用 */
    while ((e = readdir(d))) {
        char path[1024], buf[64];
        FILE *f;
        int vid, pid, bus, dev;

        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/idVendor", base, e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        vid = (int)strtol(buf, NULL, 16);
        snprintf(path, sizeof path, "%s/%s/idProduct", base, e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        pid = (int)strtol(buf, NULL, 16);
        if (vid != 0x1a86 || pid != 0x5512) continue;

        snprintf(path, sizeof path, "%s/%s/busnum", base, e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        bus = atoi(buf);
        snprintf(path, sizeof path, "%s/%s/devnum", base, e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        dev = atoi(buf);

        snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, dev);
        ok = (access(path, W_OK) == 0);
        break;
    }
    closedir(d);
    return ok;
}

static void cfg_load(void)
{
    FILE *f = fopen(g_cfgfile, "r");
    char ln[1024];

    if (!f) return;
    while (fgets(ln, sizeof ln, f)) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = ln, *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if      (!strcmp(k, "programmer")) snprintf(fld[F_PROG],   sizeof fld[0], "%s", v);
        else if (!strcmp(k, "chip"))       snprintf(fld[F_CHIP],   sizeof fld[0], "%s", v);
        else if (!strcmp(k, "backup"))     snprintf(fld[F_BACKUP], sizeof fld[0], "%s", v);
        else if (!strcmp(k, "image"))      snprintf(fld[F_IMAGE],  sizeof fld[0], "%s", v);
        else if (!strcmp(k, "uboot"))      snprintf(fld[F_UBOOT],  sizeof fld[0], "%s", v);
        else if (!strcmp(k, "font_ui"))    snprintf(g_fontui,  sizeof g_fontui,  "%s", v);
        else if (!strcmp(k, "font_log"))   snprintf(g_fontlog, sizeof g_fontlog, "%s", v);
        else if (!strcmp(k, "pkexec"))     pkexec_on = atoi(v);
    }
    fclose(f);
}

static void cfg_save(void)
{
    FILE *f = fopen(g_cfgfile, "w");
    if (!f) return;
    fprintf(f, "programmer=%s\n", fld[F_PROG]);
    fprintf(f, "chip=%s\n",       fld[F_CHIP]);
    fprintf(f, "backup=%s\n",     fld[F_BACKUP]);
    fprintf(f, "image=%s\n",      fld[F_IMAGE]);
    fprintf(f, "uboot=%s\n",      fld[F_UBOOT]);
    fprintf(f, "font_ui=%s\n",    g_fontui);
    fprintf(f, "font_log=%s\n",   g_fontlog);
    fprintf(f, "pkexec=%d\n",     pkexec_on);
    fclose(f);
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--askpass"))
        return askpass_main();

    /* 单实例: 已经有了就直接退出, 避免多个密码框/多个窗口 */
    gui_lock_fd = open("/tmp/flashrom-ch341a-gui.lock",
                       O_CREAT | O_RDWR, 0600);
    if (gui_lock_fd >= 0 && flock(gui_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Flashrom GUI 已经在运行\n");
        return 0;
    }

    init_paths();

    snprintf(fld[F_PROG],   sizeof fld[0], "ch341a_spi");
    fld[F_CHIP][0] = 0;                 /* 默认空 = 自动检测 */
    snprintf(fld[F_BACKUP], sizeof fld[0], "%s/backup.bin", g_appdir);
    snprintf(fld[F_IMAGE],  sizeof fld[0], "%s/16M.bin", g_appdir);
    fld[F_UBOOT][0] = 0;
    cfg_load();                       /* 配置文件里的值覆盖默认 */
    if (!ch341a_accessible() && geteuid() != 0) {
        helper_ok = helper_start();   /* pkexec 授权一次, 助手常驻 */
        if (!helper_ok) {
            sudo_prime();             /* 退路: sudo 缓存 */
            if (!use_sudo) pkexec_on = 1;
        }
    }
    if (!fld[F_UBOOT][0]) {           /* 没配置就自动找 app 目录下的 u-boot*.bin */
        DIR *d = opendir(g_appdir);
        struct dirent *e;
        if (d) {
            while ((e = readdir(d))) {
                if (!strncmp(e->d_name, "u-boot", 6) &&
                    strstr(e->d_name, ".bin")) {
                    snprintf(fld[F_UBOOT], sizeof fld[0], "%s/%s", g_appdir,
                             e->d_name);
                    break;
                }
            }
            closedir(d);
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "无法打开 X display\n"); return 1; }
    scr = DefaultScreen(dpy);
    vis = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 60, 40, W, H, 0,
                              0xF2F2F2, 0xF2F2F2);
    set_title_utf8(win, "Flashrom 编程器 · CH341A");
    {
        XSizeHints *h = XAllocSizeHints();
        if (h) {
            h->flags = PMinSize;
            h->min_width = 640; h->min_height = 460;
            XSetWMNormalHints(dpy, win, h);
            XFree(h);
        }
    }
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                            StructureNotifyMask | ButtonReleaseMask |
                            PointerMotionMask);
    XMapWindow(dpy, win);

    /* 文件选择器窗口 (先创建, 不映射) */
    pick_win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 120, 90,
                                   PICK_W, PICK_H, 0, 0xF2F2F2, 0xF2F2F2);
    set_title_utf8(pick_win, "选择文件");
    XSelectInput(dpy, pick_win, ExposureMask | ButtonPressMask |
                               KeyPressMask | StructureNotifyMask);

    gc = XCreateGC(dpy, win, 0, NULL);
    pm = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
    xd = XftDrawCreate(dpy, pm, vis, cmap);

    /* 选择器的离屏绘制目标 (必须在 pick_win 创建之后) */
    pick_pm = XCreatePixmap(dpy, pick_win, PICK_W, PICK_H, DefaultDepth(dpy, scr));
    pick_xd = XftDrawCreate(dpy, pick_pm, vis, cmap);

    if (g_fontui[0]) f_ui = XftFontOpenName(dpy, scr, g_fontui);
    if (!f_ui) f_ui = XftFontOpenName(dpy, scr, "Noto Sans CJK SC-11");
    if (!f_ui) f_ui = XftFontOpenName(dpy, scr, "Sans-11");
    if (g_fontlog[0]) f_log = XftFontOpenName(dpy, scr, g_fontlog);
    if (!f_log) f_log = XftFontOpenName(dpy, scr, "Noto Sans Mono CJK SC-10");
    if (!f_log) f_log = XftFontOpenName(dpy, scr, "Noto Sans CJK SC-10");
    if (!f_log) f_log = XftFontOpenName(dpy, scr, "DejaVu Sans Mono-10");
    if (!f_log) f_log = f_ui;
    if (!f_ui || !f_log) { fprintf(stderr, "字体加载失败\n"); return 1; }

    mkcolor(&c_text,  "#202124");
    mkcolor(&c_dim,   "#5f6368");
    mkcolor(&c_ok,    "#137333");
    mkcolor(&c_err,   "#c5221f");

    layout();

    log_add("Flashrom 编程器 (C 版, Xft 中文)");
    log_add("标签页: 备份 / 写入 / u-boot / 合成 / 日志");
    log_add("编程器: 默认 ch341a_spi; 也可填 ch347_spi / serprog:dev=... / linux_spi:dev=...");
    log_add("CH340 是串口芯片, 不能直接刷 SPI; 需配合 serprog 固件的设备");
    log_add("第一次用建议: 检测芯片 -> 备份整片留底");
    if (geteuid() == 0)
        log_add("权限: 以 root 运行");
    else if (helper_ok)
        log_add("权限: 已授权一次(助手常驻), 本次运行不再询问");
    else if (use_sudo)
        log_add("权限: 已用 sudo 提权(启动时问过一次), 本次运行不再询问");
    else if (ch341a_accessible())
        log_add("权限: CH341A 直接可访问(无需提权)");
    else
        log_add("权限: 每次操作需 pkexec 授权(未安装 udev 规则)");
    log_add("路径/芯片/字体可改, 退出时自动存到:");
    log_add(g_cfgfile);
    log_add("日志页可拖动选中文本, Ctrl+C 或点“复制选中”复制到剪贴板");

    Atom wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmdel, 1);
    redraw();

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        struct pollfd p[2];
        int np = 0;
        p[np].fd = xfd; p[np].events = POLLIN; np++;
        if (pfp) { p[np].fd = fileno(pfp); p[np].events = POLLIN; np++; }
        if (poll(p, np, 200) < 0) continue;

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.window == pick_win) { pick_draw(); break; }
                if (ev.xexpose.count == 0) redraw();
                break;
            case ButtonPress:
                if (ev.xbutton.window == win) {
                    int ln = log_hit_line(ev.xbutton.y);
                    if (ln >= 0 && ev.xbutton.button == 1) {
                        dragging = 1;
                        sel_a = sel_b = ln;
                        redraw();
                        break;
                    }
                }
                if (ev.xbutton.window == pick_win) {
                    if (ev.xbutton.button == Button4) {
                        pick_scroll -= 3;
                        if (pick_scroll < 0) pick_scroll = 0;
                        if (pick_sel > pick_scroll + 11) pick_sel = pick_scroll + 11;
                        if (pick_sel < pick_scroll) pick_sel = pick_scroll;
                        pick_draw();
                    } else if (ev.xbutton.button == Button5) {
                        pick_scroll += 3;
                        if (pick_scroll > pick_n - 1) pick_scroll = pick_n - 1;
                        if (pick_sel < pick_scroll) pick_sel = pick_scroll;
                        pick_draw();
                    } else if (ev.xbutton.button == 1) {
                        pick_click(ev.xbutton.x, ev.xbutton.y);
                    }
                    break;
                }
                if (ev.xbutton.button == Button4) {
                    if (nlines) {
                        if (log_off < 0) log_off = nlines - 6;
                        log_off -= 3;
                        if (log_off < 0) log_off = 0;
                        redraw();
                    }
                } else if (ev.xbutton.button == Button5) {
                    if (log_off >= 0) { log_off += 3; redraw(); }
                } else {
                    on_click(ev.xbutton.x, ev.xbutton.y);
                }
                break;
            case ButtonRelease:
                dragging = 0;
                break;
            case MotionNotify:
                if (dragging) {
                    int ln = log_hit_line(ev.xmotion.y);
                    if (ln >= 0 && ln != sel_b) { sel_b = ln; redraw(); }
                }
                break;
            case KeyPress:
                if (pick_on) {
                    KeySym k = XLookupKeysym(&ev.xkey, 0);
                    char kb[32];
                    int kn = XLookupString(&ev.xkey, kb, sizeof kb, &k, NULL);
                    if (pick_editing) {
                        size_t el = strlen(pick_edit);
                        if (k == XK_Escape) { pick_editing = 0; pick_draw(); break; }
                        if (k == XK_Return) {
                            pick_editing = 0;
                            pick_load(pick_edit);
                            pick_draw();
                            break;
                        }
                        if (k == XK_BackSpace) {
                            if (el) {
                                el--;
                                while (el && ((unsigned char)pick_edit[el] & 0xC0) == 0x80) el--;
                                pick_edit[el] = 0;
                            }
                            pick_draw();
                            break;
                        }
                        if (kn > 0 && pick_fresh) {
                            pick_edit[0] = 0; el = 0; pick_fresh = 0;
                        }
                        if (kn > 0 && el + (size_t)kn < sizeof pick_edit - 1) {
                            memcpy(pick_edit + el, kb, kn);
                            pick_edit[el + kn] = 0;
                            pick_draw();
                        }
                        break;
                    }
                    if (k == XK_Escape) { pick_close(); break; }
                    if (k == XK_Up && pick_sel > 0) { pick_sel--; pick_draw(); break; }
                    if (k == XK_Down && pick_sel < pick_n - 1) {
                        pick_sel++; pick_draw(); break;
                    }
                    if (k == XK_Page_Up) { pick_sel -= 10;
                        if (pick_sel < 0) pick_sel = 0; pick_draw(); break; }
                    if (k == XK_Page_Down) { pick_sel += 10;
                        if (pick_sel > pick_n - 1) pick_sel = pick_n - 1;
                        pick_draw(); break; }
                    if (k == XK_Return) {
                        pick_click(30, PICK_H - 20);   /* = 打开 */
                        break;
                    }
                    break;
                }
                if ((ev.xkey.state & ControlMask) &&
                    XLookupKeysym(&ev.xkey, 0) == XK_c) {
                    copy_selection();
                    break;
                }
                on_key(&ev.xkey);
                break;
            case ConfigureNotify:
                redraw();
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wmdel) {
                    stop_child();
                    helper_stop();
                    cfg_save();
                    return 0;
                }
                break;
            default:
                break;
            }
        }
        sudo_keepalive();
        if (helper_busy) {
            g_pct = g_pct;
            helper_poll();
        } else if (running && helper_ok) {
            running = 0;
        }
        if (pfp) pump_child();
    }
}
