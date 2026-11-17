// HA Switch for TRIMUI BRICK Pro
// 4x5 のスイッチを並べ、十字キーで選んで A で HA の Webhook に POST する。
// framebuffer (/dev/fb0) と evdev に直接アクセスする静的リンクのバイナリ。
// ビルド: src/build.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NEON
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include "keys.h"

#define COLS 5
#define ROWS 4
#define NSLOT (COLS * ROWS)
#define MAXPAGE 6

// ---------- 色 (0xAARRGGBB) ----------
#define C_BG        0xFF101418
#define C_TILE      0xFF1E252D
#define C_TILE_SEL  0xFF2C3E50
#define C_BORDER    0xFF03A9F4
#define C_TEXT      0xFFECEFF1
#define C_SUB       0xFF90A4AE
#define C_ICON      0xFFFFC107
#define C_OK        0xFF2E7D32
#define C_NG        0xFFC62828
#define C_BUSY      0xFF455A64
#define C_ICON_OFF  0xFF546E7A
#define C_TEXT_OFF  0xFF78909C
#define C_ICON_BTN  0xFF4FC3F7
#define C_ICON_NA   0xFF8D6E63

typedef struct {
    int used;
    char label[64];
    char icon[64];
    char entity[128];
    int cp;            // アイコンのコードポイント
    int cp_off;        // OFF 時のアイコン（0 なら cp と同じ）
    char stent[128];   // 状態を見る entity（4列目。省略時は entity）
    int st;            // 0=状態なし(ボタン) 1=ON 2=OFF 3=不明 4=未取得
    int state;         // 0=なし 1=送信中 2=OK 3=NG
    double state_t;
} Slot;

static Slot slots[MAXPAGE * NSLOT];
static char page_name[MAXPAGE][64];
static char page_cam[MAXPAGE][64];   // カメラページなら go2rtc のストリーム名
static int npages = 1, pg;   // ページ数、表示中のページ
static char webhook_url[256];
static char appdir[256];
static char status_msg[128];
static char api_base[200];
static char cam_base[200];   // go2rtc（camurl=）
static char token[512];
static int poll_ng;          // 状態取得の連続失敗回数
static double next_poll;     // 次に状態を取得する時刻
static double alert_until;   // アラート（config の alert= のタイマー）の終了時刻。0 なら無し
static char alert_timer[128];  // alert=（空なら赤点滅なし）
static char xbutton_ent[128];  // xbutton=（X ボタンで実行する entity）
static double status_t;

static int W, H, fbfd = -1;
static uint32_t *fbmem, *bb;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static stbtt_fontinfo font_txt, font_icon;
static volatile int quit_flag;
static int screen_off;
static int cur;            // 選択中のスロット（ページ内の番号）
static int last_fired = -1;

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void on_sig(int s) { (void)s; quit_flag = 1; }

// ---------- ファイル ----------
static unsigned char *load_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(n + 1);
    if (fread(p, 1, n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
    fclose(f); p[n] = 0;
    if (len) *len = n;
    return p;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int icon_cp(const char *name) {
    if (!strncmp(name, "mdi:", 4)) name += 4;
    char path[300]; snprintf(path, sizeof path, "%s/icons.txt", appdir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[200]; int cp = 0;
    size_t nl = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, name, nl) && line[nl] == ' ') { cp = (int)strtol(line + nl + 1, NULL, 16); break; }
    }
    fclose(f);
    return cp;
}

static void load_config(void) {
    memset(slots, 0, sizeof slots);
    memset(page_name, 0, sizeof page_name);
    memset(page_cam, 0, sizeof page_cam);
    strcpy(webhook_url, "http://homeassistant.local:8123/api/webhook/brick_switch");
    strcpy(cam_base, "http://homeassistant.local:1984");
    alert_timer[0] = 0;
    strcpy(xbutton_ent, "script.all_toggle");
    char path[300]; snprintf(path, sizeof path, "%s/config.txt", appdir);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(status_msg, sizeof status_msg, "config.txt がありません"); status_t = now(); return; }
    char line[512];
    int pi = 0, k = 0;   // 書き込み中のページとページ内の位置
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "url=", 4)) { snprintf(webhook_url, sizeof webhook_url, "%s", s + 4); continue; }
        if (!strncmp(s, "camurl=", 7)) { snprintf(cam_base, sizeof cam_base, "%s", s + 7); continue; }
        if (!strncmp(s, "alert=", 6)) { snprintf(alert_timer, sizeof alert_timer, "%s", s + 6); continue; }
        if (!strncmp(s, "xbutton=", 8)) { snprintf(xbutton_ent, sizeof xbutton_ent, "%s", s + 8); continue; }
        if (*s == '[') {   // [ページ名] で新しいページ
            if (k > 0 || page_name[pi][0]) { pi++; k = 0; }
            if (pi >= MAXPAGE) break;
            char *e = strchr(s, ']'); if (e) *e = 0;
            snprintf(page_name[pi], sizeof page_name[pi], "%s", s + 1);
            continue;
        }
        if (!strncmp(s, "camera=", 7)) {   // このページをカメラページにする
            if (k > 0) { pi++; k = 0; if (pi >= MAXPAGE) break; }
            snprintf(page_cam[pi], sizeof page_cam[pi], "%s", s + 7);
            if (!page_name[pi][0]) snprintf(page_name[pi], sizeof page_name[pi], "%s", s + 7);
            continue;
        }
        if (k == NSLOT) { pi++; k = 0; if (pi >= MAXPAGE) break; }   // 20個を超えたら次のページへ
        if (!strcmp(s, "-")) { k++; continue; }
        char *a = strtok(s, "|"), *b = strtok(NULL, "|"), *c = strtok(NULL, "|"), *d = strtok(NULL, "|");
        if (!a || !b || !c) continue;
        Slot *sl = &slots[pi * NSLOT + k++];
        sl->used = 1;
        snprintf(sl->label, sizeof sl->label, "%s", trim(a));
        snprintf(sl->icon, sizeof sl->icon, "%s", trim(b));
        snprintf(sl->entity, sizeof sl->entity, "%s", trim(c));
        sl->cp = icon_cp(sl->icon);
        if (!sl->cp) sl->cp = icon_cp("help-circle-outline");
        snprintf(sl->stent, sizeof sl->stent, "%s", d ? trim(d) : sl->entity);
        if (!strncmp(sl->entity, "lock.", 5)) sl->cp_off = icon_cp("lock-open-variant");
        sl->st = (strncmp(sl->stent, "scene.", 6) && strncmp(sl->stent, "script.", 7) && strncmp(sl->stent, "button.", 7)
                  && strncmp(sl->stent, "cam:", 4)) ? 4 : 0;
    }
    fclose(f);
    npages = (pi < MAXPAGE) ? pi + 1 : MAXPAGE;
    if (npages > 1 && k == 0 && !page_name[npages - 1][0]) npages--;
    if (pg >= npages) pg = 0;
    next_poll = 0;
    // API のベース URL（webhook の URL の /api/ より前）
    snprintf(api_base, sizeof api_base, "%s", webhook_url);
    char *q = strstr(api_base, "/api/"); if (q) *q = 0;
    // トークン
    token[0] = 0;
    snprintf(path, sizeof path, "%s/token", appdir);
    unsigned char *tk = load_file(path, NULL);
    if (tk) { snprintf(token, sizeof token, "%s", trim((char *)tk)); free(tk); }
}

// ---------- 描画 ----------
static inline void blend(uint32_t *p, uint32_t col, int a) {
    if (a <= 0) return;
    if (a >= 255) { *p = col; return; }
    uint32_t d = *p;
    int r = (((col >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (((col >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = ((col & 255) * a + (d & 255) * (255 - a)) / 255;
    *p = 0xFF000000 | (r << 16) | (g << 8) | b;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < W) bb[j * W + i] = c;
    }
}

// 玄関アラート: 作動中は画面の縁に赤い枠を 0.4 秒ごとに点滅させる（HA の dashboard-wall / xperia と同じ）
#define ALERT_PX 16
static int alert_visible(void) {
    double t = now();
    return t < alert_until && ((long)(t / 0.4) & 1) == 0;
}
static void draw_alert(void) {
    if (!alert_visible()) return;
    for (int y = 0; y < H; y++) {
        int edge = y < ALERT_PX || y >= H - ALERT_PX;
        for (int x = 0; x < W; x++)
            if (edge || x < ALERT_PX || x >= W - ALERT_PX) blend(&bb[y * W + x], 0xFFFF1A1A, 160);
    }
}

// 角丸矩形（r: 半径）
static void round_rect(int x, int y, int w, int h, int r, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i; if (xx < 0 || xx >= W) continue;
            int dx = 0, dy = 0;
            if (i < r) dx = r - i; else if (i >= w - r) dx = i - (w - r - 1);
            if (j < r) dy = r - j; else if (j >= h - r) dy = j - (h - r - 1);
            if (dx && dy) {
                float d = sqrtf((float)(dx * dx + dy * dy)) - r;
                if (d > 1) continue;
                if (d > 0) { blend(&bb[yy * W + xx], c, (int)((1 - d) * 255)); continue; }
            }
            bb[yy * W + xx] = c;
        }
    }
}

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0) { *s += 2; return ((c & 31) << 6) | (p[1] & 63); }
    if ((c & 0xF0) == 0xE0) { *s += 3; return ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); }
    *s += 4; return ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
}

// px: フォントの高さ(ピクセル)。draw=0 なら幅だけ返す
static int text(stbtt_fontinfo *fi, float px, int x, int y, const char *s, uint32_t col, int draw) {
    float sc = stbtt_ScaleForPixelHeight(fi, px);
    int asc, desc, gap; stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    int base = y + (int)(asc * sc);
    float pen = x;
    int prev = 0;
    while (*s) {
        int cp = utf8_next(&s);
        if (prev) pen += sc * stbtt_GetCodepointKernAdvance(fi, prev, cp);
        int adv, lsb; stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
        if (draw) {
            int gw, gh, xo, yo;
            unsigned char *bm = stbtt_GetCodepointBitmap(fi, sc, sc, cp, &gw, &gh, &xo, &yo);
            if (bm) {
                for (int j = 0; j < gh; j++) {
                    int yy = base + yo + j; if (yy < 0 || yy >= H) continue;
                    for (int i = 0; i < gw; i++) {
                        int xx = (int)pen + xo + i; if (xx < 0 || xx >= W) continue;
                        blend(&bb[yy * W + xx], col, bm[j * gw + i]);
                    }
                }
                stbtt_FreeBitmap(bm, NULL);
            }
        }
        pen += adv * sc;
        prev = cp;
    }
    return (int)(pen - x);
}

static void text_center(stbtt_fontinfo *fi, float px, int cx, int y, const char *s, uint32_t col) {
    int w = text(fi, px, 0, 0, s, col, 0);
    text(fi, px, cx - w / 2, y, s, col, 1);
}

static void icon_center(int cp, float px, int cx, int y, uint32_t col) {
    char buf[8]; int n = 0;
    // コードポイント → UTF-8
    if (cp < 0x10000) { buf[n++] = 0xE0 | (cp >> 12); buf[n++] = 0x80 | ((cp >> 6) & 63); buf[n++] = 0x80 | (cp & 63); }
    else { buf[n++] = 0xF0 | (cp >> 18); buf[n++] = 0x80 | ((cp >> 12) & 63); buf[n++] = 0x80 | ((cp >> 6) & 63); buf[n++] = 0x80 | (cp & 63); }
    buf[n] = 0;
    text_center(&font_icon, px, cx, y, buf, col);
}

#define HEAD_H 56
#define FOOT_H 40
#define GAP 10

static void draw(void) {
    fill_rect(0, 0, W, H, C_BG);

    // ヘッダー
    icon_center(icon_cp("home-assistant"), 40, 36, 6, C_BORDER);
    int hw = text(&font_txt, 30, 60, 12, "Home Assistant", C_TEXT, 1);
    if (page_name[pg][0]) text(&font_txt, 26, 60 + hw + 18, 15, page_name[pg], C_SUB, 1);
    char tbuf[32]; time_t t = time(NULL); struct tm *tm = localtime(&t);
    strftime(tbuf, sizeof tbuf, "%H:%M", tm);
    int tw = text(&font_txt, 30, 0, 0, tbuf, 0, 0);
    text(&font_txt, 30, W - 16 - tw, 12, tbuf, C_TEXT, 1);
    if (!token[0] || poll_ng >= 3) {
        const char *m = !token[0] ? "状態表示なし(token未設定)" : "HA に接続できません";
        int sw = text(&font_txt, 24, 0, 0, m, 0, 0);
        text(&font_txt, 24, W - 40 - tw - sw, 16, m, C_NG | 0xFF000000, 1);
    } else if (status_msg[0] && now() - status_t < 4) {
        int sw = text(&font_txt, 24, 0, 0, status_msg, 0, 0);
        text(&font_txt, 24, W - 40 - tw - sw, 16, status_msg, C_SUB, 1);
    }

    // タイル
    int tw_ = (W - GAP * (COLS + 1)) / COLS;
    int th_ = (H - HEAD_H - FOOT_H - GAP * (ROWS + 1)) / ROWS;
    double tn = now();
    for (int i = 0; i < NSLOT; i++) {
        int r = i / COLS, c = i % COLS;
        int x = GAP + c * (tw_ + GAP), y = HEAD_H + GAP + r * (th_ + GAP);
        Slot *s = &slots[pg * NSLOT + i];
        uint32_t bg = (i == cur) ? C_TILE_SEL : C_TILE;
        if (s->state == 1) bg = C_BUSY;
        else if (s->state == 2 && tn - s->state_t < 0.8) bg = C_OK;
        else if (s->state == 3 && tn - s->state_t < 2.0) bg = C_NG;
        if (i == cur) round_rect(x - 4, y - 4, tw_ + 8, th_ + 8, 18, C_BORDER);
        round_rect(x, y, tw_, th_, 14, bg);
        if (!s->used) continue;
        uint32_t ic = C_ICON, tc = C_TEXT; int cp = s->cp;
        if (s->st == 0) ic = C_ICON_BTN;
        else if (s->st == 2) { ic = C_ICON_OFF; tc = C_TEXT_OFF; if (s->cp_off) cp = s->cp_off; }
        else if (s->st == 3) { ic = C_ICON_NA; tc = C_TEXT_OFF; }
        else if (s->st == 4) ic = C_ICON_OFF;
        icon_center(cp, 76, x + tw_ / 2, y + 14, ic);
        text_center(&font_txt, 30, x + tw_ / 2, y + th_ - 46, s->label, tc);
    }

    // フッター（操作説明）
    text(&font_txt, 22, 16, H - FOOT_H + 8,
         "十字:選択  A:実行  Y:もう一度  X:全照明  SELECT:画面OFF  START:再読込  B:終了",
         C_SUB, 1);
    void draw_pager(void); draw_pager();
    draw_alert();
}

// 右下: ページ表示  ‹ L  1/2  R ›
void draw_pager(void) {
    if (npages > 1) {
        char pb[32]; snprintf(pb, sizeof pb, "%d/%d", pg + 1, npages);
        int fy = H - FOOT_H + 4;
        int pw = text(&font_txt, 28, 0, 0, pb, 0, 0);
        int xr = W - 16;
        icon_center(icon_cp("chevron-right"), 32, xr - 14, fy, C_BORDER); xr -= 30;
        int rw = text(&font_txt, 24, 0, 0, "R", 0, 0);
        text(&font_txt, 24, xr - rw, fy + 4, "R", C_SUB, 1); xr -= rw + 12;
        text(&font_txt, 28, xr - pw, fy + 1, pb, C_TEXT, 1); xr -= pw + 12;
        int lw = text(&font_txt, 24, 0, 0, "L", 0, 0);
        text(&font_txt, 24, xr - lw, fy + 4, "L", C_SUB, 1); xr -= lw + 2;
        icon_center(icon_cp("chevron-left"), 32, xr - 14, fy, C_BORDER);
    }
}

static void present(void) {
    if (fbfd < 0) return;   // HS_DUMP（確認用）では描画しない
    uint8_t *dst = (uint8_t *)fbmem;
    for (int y = 0; y < H; y++) memcpy(dst + y * finfo.line_length, bb + y * W, W * 4);
    vinfo.yoffset = 0; vinfo.xoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);
}

// ---------- Webhook ----------
#define MAXJOB 8
static struct { pid_t pid; int slot; } jobs[MAXJOB];

static void fire(int i) {   // i は全ページ通しの番号
    if (i < 0 || i >= MAXPAGE * NSLOT || !slots[i].used) return;
    Slot *s = &slots[i];
    if (!strncmp(s->entity, "cam:", 4)) {   // そのカメラのページへ移動
        void set_page(int);
        for (int p = 0; p < npages; p++) if (!strcmp(page_cam[p], s->entity + 4)) { set_page(p); return; }
        return;
    }
    if (s->state == 1) return;  // 送信中
    int k; for (k = 0; k < MAXJOB && jobs[k].pid; k++);
    if (k == MAXJOB) return;
    char body[256]; snprintf(body, sizeof body, "{\"entity\":\"%s\"}", s->entity);
    pid_t p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
        execl("/usr/bin/curl", "curl", "-s", "--fail", "-m", "5", "-X", "POST",
              "-H", "Content-Type: application/json", "-d", body, webhook_url, (char *)NULL);
        _exit(127);
    }
    if (p < 0) return;
    jobs[k].pid = p; jobs[k].slot = i;
    s->state = 1; s->state_t = now();
    last_fired = i;
}

static void reap(void) {
    for (int k = 0; k < MAXJOB; k++) {
        if (!jobs[k].pid) continue;
        int st;
        if (waitpid(jobs[k].pid, &st, WNOHANG) == jobs[k].pid) {
            Slot *s = &slots[jobs[k].slot];
            int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            s->state = ok ? 2 : 3; s->state_t = now();
            if (ok) next_poll = now() + 0.7;  // 操作直後は早めに状態を取り直す
            snprintf(status_msg, sizeof status_msg, "%s %s", s->label, ok ? "OK" : "送信失敗");
            status_t = now();
            jobs[k].pid = 0;
        }
    }
}

// ---------- 状態取得（/api/template で20個まとめて） ----------
static pid_t poll_pid;
static int poll_fd = -1;
static char poll_buf[4096];
static int poll_len;
static int poll_page;


static int stateful(const Slot *s) {
    return strncmp(s->stent, "scene.", 6) && strncmp(s->stent, "script.", 7) && strncmp(s->stent, "button.", 7)
           && strncmp(s->stent, "cam:", 4);
}

static void poll_start(void) {
    if (poll_pid || !token[0]) return;
    char tpl[4096] = "{\"template\":\"";
    poll_page = pg;
    Slot *ps = &slots[pg * NSLOT];
    for (int i = 0; i < NSLOT; i++) {
        char part[200];
        if (ps[i].used && stateful(&ps[i])) snprintf(part, sizeof part, "{{ states('%s') }}|", ps[i].stent);
        else snprintf(part, sizeof part, "-|");
        strncat(tpl, part, sizeof tpl - strlen(tpl) - 4);
    }
    // 最後の項目: アラート用タイマーの残り秒数（作動中でなければ 0）
    if (alert_timer[0]) {
        char at[400];
        snprintf(at, sizeof at, "{{ ((as_timestamp(state_attr('%s','finishes_at'), 0) - as_timestamp(now())) | round(1))"
                 " if is_state('%s','active') else 0 }}", alert_timer, alert_timer);
        strncat(tpl, at, sizeof tpl - strlen(tpl) - 4);
    } else strncat(tpl, "0", sizeof tpl - strlen(tpl) - 4);
    strcat(tpl, "\"}");
    char url[300]; snprintf(url, sizeof url, "%s/api/template", api_base);
    char auth[600]; snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    int pp[2]; if (pipe(pp)) return;
    pid_t p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(pp[1], 1); dup2(dn, 2); close(pp[0]);
        execl("/usr/bin/curl", "curl", "-s", "--fail", "-m", "4", "-X", "POST",
              "-H", auth, "-H", "Content-Type: application/json", "-d", tpl, url, (char *)NULL);
        _exit(127);
    }
    close(pp[1]);
    if (p < 0) { close(pp[0]); return; }
    poll_pid = p; poll_fd = pp[0]; poll_len = 0;
    fcntl(poll_fd, F_SETFL, O_NONBLOCK);
}

static int poll_step(void) {  // 状態が更新されたら 1
    if (!poll_pid) return 0;
    for (;;) {
        int r = read(poll_fd, poll_buf + poll_len, sizeof poll_buf - 1 - poll_len);
        if (r > 0) { poll_len += r; continue; }
        break;
    }
    int st;
    if (waitpid(poll_pid, &st, WNOHANG) != poll_pid) return 0;
    int r; while ((r = read(poll_fd, poll_buf + poll_len, sizeof poll_buf - 1 - poll_len)) > 0) poll_len += r;
    close(poll_fd); poll_fd = -1; poll_pid = 0;
    poll_buf[poll_len] = 0;
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) { poll_ng++; return 1; }
    poll_ng = 0;
    char *p = poll_buf;
    for (int i = 0; i < NSLOT && p; i++) {
        char *e = strchr(p, '|'); if (e) *e = 0;
        Slot *s = &slots[poll_page * NSLOT + i];
        if (!s->used || !stateful(s)) s->st = 0;
        else if (!strcmp(p, "on") || !strcmp(p, "locked") || !strcmp(p, "open") || !strcmp(p, "playing") || !strcmp(p, "home")) s->st = 1;
        else if (!strcmp(p, "unavailable") || !strcmp(p, "unknown")) s->st = 3;
        else s->st = 2;
        p = e ? e + 1 : NULL;
    }
    if (p) { double rem = atof(p); alert_until = rem > 0 ? now() + rem : 0; }
    return 1;
}

// ---------- カメラ（go2rtc の MJPEG を全画面表示） ----------
static int viewing = -1;          // 表示中のカメラページ。-1 ならカメラ非表示
static pid_t cam_pid;
static int cam_fd = -1;
static unsigned char *cam_buf;
static int cam_len;
#define CAM_BUFSZ (4 << 20)
static double cam_last_frame, cam_retry_t, cam_open_t;
static int cam_frames; static double cam_fps_t; static float cam_fps;

static void cam_stop(void) {
    if (cam_pid) { kill(cam_pid, SIGTERM); waitpid(cam_pid, NULL, 0); cam_pid = 0; }
    if (cam_fd >= 0) { close(cam_fd); cam_fd = -1; }
    cam_len = 0;
}

static void cam_start(void) {
    cam_stop();
    if (viewing < 0) return;
    char url[400];
    snprintf(url, sizeof url, "%s/api/stream.mjpeg?src=%s", cam_base, page_cam[viewing]);
    int pp[2]; if (pipe(pp)) return;
    pid_t p = fork();
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(pp[1], 1); dup2(dn, 2); close(pp[0]);
        execl("/usr/bin/curl", "curl", "-s", "-N", "--fail", "--connect-timeout", "4", url, (char *)NULL);
        _exit(127);
    }
    close(pp[1]);
    if (p < 0) { close(pp[0]); return; }
    cam_pid = p; cam_fd = pp[0]; cam_len = 0;
    fcntl(cam_fd, F_SETFL, O_NONBLOCK);
    cam_open_t = now();
}

static void cam_overlay(const char *msg) {
    fill_rect(0, 0, W, 60, 0xFF000000);
    fill_rect(0, H - FOOT_H, W, FOOT_H, 0xFF000000);
    icon_center(icon_cp("cctv"), 40, 36, 8, C_ICON_BTN);
    text(&font_txt, 30, 64, 12, page_name[viewing], C_TEXT, 1);
    char tb[64];
    if (msg) snprintf(tb, sizeof tb, "%s", msg); else snprintf(tb, sizeof tb, "%.1f fps", cam_fps);
    int w = text(&font_txt, 24, 0, 0, tb, 0, 0);
    text(&font_txt, 24, W - 16 - w, 16, tb, msg ? C_NG : C_SUB, 1);
    text(&font_txt, 22, 16, H - FOOT_H + 8, "十字←→/LR:ページ切替  X:全照明  SELECT:画面OFF  B:終了", C_SUB, 1);
    void draw_pager(void); draw_pager();
}

void cam_open(int p) {
    viewing = p;
    cam_frames = 0; cam_fps = 0; cam_fps_t = now(); cam_last_frame = 0;
    fill_rect(0, 0, W, H, 0xFF000000);
    cam_overlay("接続中...");
    present();
    cam_start();
}

static void cam_close(void) { cam_stop(); viewing = -1; next_poll = 0; }


// 1枚描画したら 1
static int cam_step(void) {
    double t = now();
    if (cam_fd < 0) {
        if (t > cam_retry_t) cam_start();
        return 0;
    }
    for (;;) {
        if (cam_len >= CAM_BUFSZ) cam_len = 0;   // 異常に大きいときは捨てる
        int r = read(cam_fd, cam_buf + cam_len, CAM_BUFSZ - cam_len);
        if (r > 0) { cam_len += r; continue; }
        if (r == 0) {   // curl が終了した → 少し待って再接続
            cam_stop(); cam_retry_t = t + 1.5;
            fill_rect(0, 0, W, H, 0xFF000000); cam_overlay("再接続中..."); present();
            return 0;
        }
        break;
    }
    // 最後の完全な JPEG（FFD8 ... FFD9）を探す
    int fs = -1, fe = -1, pos = 0, consumed = 0;
    for (;;) {
        int s0 = -1;
        for (int k = pos; k + 2 < cam_len; k++) if (cam_buf[k] == 0xFF && cam_buf[k + 1] == 0xD8 && cam_buf[k + 2] == 0xFF) { s0 = k; break; }
        if (s0 < 0) break;
        int e0 = -1;
        for (int k = s0 + 2; k + 1 < cam_len; k++) if (cam_buf[k] == 0xFF && cam_buf[k + 1] == 0xD9) { e0 = k + 2; break; }
        if (e0 < 0) { consumed = s0; break; }
        fs = s0; fe = e0; pos = e0; consumed = e0;
    }
    int drew = 0;
    if (fs >= 0) {
        int iw, ih, nc;
        unsigned char *img = stbi_load_from_memory(cam_buf + fs, fe - fs, &iw, &ih, &nc, 3);
        if (img) {
            // 縦横比を保って画面に収める（双線形補間）
            int dw = W, dh = (int)((long)W * ih / iw);
            if (dh > H) { dh = H; dw = (int)((long)H * iw / ih); }
            int ox = (W - dw) / 2, oy = (H - dh) / 2;
            fill_rect(0, 0, W, H, 0xFF000000);
            for (int y = 0; y < dh; y++) {
                float sy = (y + 0.5f) * ih / dh - 0.5f; if (sy < 0) sy = 0;
                int y0 = (int)sy, y1 = y0 + 1 < ih ? y0 + 1 : y0; int fy = (int)((sy - y0) * 256);
                uint32_t *row = bb + (oy + y) * W + ox;
                for (int x = 0; x < dw; x++) {
                    float sx = (x + 0.5f) * iw / dw - 0.5f; if (sx < 0) sx = 0;
                    int x0 = (int)sx, x1 = x0 + 1 < iw ? x0 + 1 : x0; int fx = (int)((sx - x0) * 256);
                    const unsigned char *a = img + (y0 * iw + x0) * 3, *b = img + (y0 * iw + x1) * 3;
                    const unsigned char *c = img + (y1 * iw + x0) * 3, *d = img + (y1 * iw + x1) * 3;
                    int px[3];
                    for (int ch = 0; ch < 3; ch++) {
                        int top = a[ch] * (256 - fx) + b[ch] * fx, bot = c[ch] * (256 - fx) + d[ch] * fx;
                        px[ch] = (top * (256 - fy) + bot * fy) >> 16;
                    }
                    row[x] = 0xFF000000 | (px[0] << 16) | (px[1] << 8) | px[2];
                }
            }
            stbi_image_free(img);
            cam_frames++; cam_last_frame = t;
            if (t - cam_fps_t >= 1.0) { cam_fps = cam_frames / (t - cam_fps_t); cam_frames = 0; cam_fps_t = t; }
            cam_overlay(NULL);
            draw_alert();
            drew = 1;
        }
    }
    if (consumed > 0) { memmove(cam_buf, cam_buf + consumed, cam_len - consumed); cam_len -= consumed; }
    if (!drew && t - (cam_last_frame ? cam_last_frame : cam_open_t) > 8) {   // 8秒映像が来なければ再接続
        cam_stop(); cam_retry_t = t + 0.5;
        fill_rect(0, 0, W, H, 0xFF000000); cam_overlay("映像なし（再接続中）"); present();
        cam_last_frame = 0; cam_open_t = t;
    }
    return drew;
}

// ---------- 画面 ON/OFF ----------
static void set_screen(int off) {
    screen_off = off;
    if (viewing >= 0) { if (off) cam_stop(); else { cam_retry_t = 0; cam_open_t = now(); cam_last_frame = 0; } }
    ioctl(fbfd, FBIOBLANK, off ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
}

// ---------- 入力 ----------
void set_page(int p) {
    pg = (p + npages) % npages;
    next_poll = 0;   // すぐ状態を取りに行く
    if (page_cam[pg][0]) cam_open(pg);          // カメラページなら受信開始
    else if (viewing >= 0) cam_close();         // それ以外なら受信停止
}

static void move(int dx, int dy) {
    int r = cur / COLS, c = cur % COLS;
    // 複数ページのときは、左右の端で隣のページへ
    if (npages > 1 && dx > 0 && c == COLS - 1) { set_page(pg + 1); cur = r * COLS; return; }
    if (npages > 1 && dx < 0 && c == 0) { set_page(pg - 1); cur = r * COLS + COLS - 1; return; }
    c = (c + dx + COLS) % COLS;
    r = (r + dy + ROWS) % ROWS;
    cur = r * COLS + c;
}

static int find_entity(const char *e) {
    for (int i = 0; i < MAXPAGE * NSLOT; i++) if (slots[i].used && !strcmp(slots[i].entity, e)) return i;
    return -1;
}


// 論理ボタンの押下（rep=1 はキーリピート）
static void on_button(int b, int rep) {
    if (screen_off) { if (!rep) set_screen(0); return; }  // 画面OFF中は点灯だけ
    if (viewing >= 0) {   // カメラ表示中
        switch (b) {
        case BTN_L_LEFT: case BTN_L_L1:  if (!rep) set_page(pg - 1); break;
        case BTN_L_RIGHT: case BTN_L_R1: if (!rep) set_page(pg + 1); break;
        case BTN_L_X:     if (!rep) fire(find_entity(xbutton_ent)); break;
        case BTN_L_Y:     if (!rep && last_fired >= 0) fire(last_fired); break;
        case BTN_L_SELECT: if (!rep) set_screen(1); break;
        case BTN_L_START: if (!rep) { load_config(); set_page(pg); } break;
        case BTN_L_B: case BTN_L_MENU: quit_flag = 1; break;
        }
        return;
    }
    switch (b) {
    case BTN_L_UP:    move(0, -1); break;
    case BTN_L_DOWN:  move(0, 1); break;
    case BTN_L_LEFT:  move(-1, 0); break;
    case BTN_L_RIGHT: move(1, 0); break;
    case BTN_L_L1:    if (!rep) set_page(pg - 1); break;
    case BTN_L_R1:    if (!rep) set_page(pg + 1); break;
    case BTN_L_A:     if (!rep) fire(pg * NSLOT + cur); break;
    case BTN_L_Y:     if (!rep && last_fired >= 0) fire(last_fired); break;
    case BTN_L_X:     if (!rep) fire(find_entity(xbutton_ent)); break;
    case BTN_L_SELECT:if (!rep) set_screen(1); break;
    case BTN_L_START: if (!rep) { load_config(); snprintf(status_msg, sizeof status_msg, "設定を再読込"); status_t = now(); } break;
    case BTN_L_B:     if (!rep) quit_flag = 1; break;
    case BTN_L_MENU:  quit_flag = 1; break;
    }
}

int main(int argc, char **argv) {
    snprintf(appdir, sizeof appdir, "%s", argc > 1 ? argv[1] : ".");
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
    setenv("TZ", "JST-9", 0);
    tzset();

    // 確認用: HS_DUMP=出力.ppm を指定すると、fb を使わず1枚描画してファイルに書き出す（Pi5 上でも動く）
    const char *dump = getenv("HS_DUMP");
    if (dump) {
        W = 1024; H = 768; bb = malloc(W * H * 4);
        long n;
        const char *fp = getenv("HS_FONT") ? getenv("HS_FONT") : "/usr/trimui/res/full.ttf";
        unsigned char *ft = load_file(fp, &n);
        char ip[300]; snprintf(ip, sizeof ip, "%s/mdi.ttf", appdir);
        unsigned char *fi = load_file(ip, &n);
        if (!ft || !fi) { fprintf(stderr, "font load failed\n"); return 1; }
        stbtt_InitFont(&font_txt, ft, stbtt_GetFontOffsetForIndex(ft, 0));
        stbtt_InitFont(&font_icon, fi, 0);
        load_config();
        if (getenv("HS_PAGE")) pg = atoi(getenv("HS_PAGE"));
        if (getenv("HS_CAM")) {   // カメラの確認: HS_CAM=スロット番号 で10秒受信し、fps と最後のコマを出力
            cam_buf = malloc(CAM_BUFSZ);
            pg = atoi(getenv("HS_CAM")); cam_open(pg);
            double t0 = now(); int nf = 0;
            while (now() - t0 < 10) { struct pollfd pf = { cam_fd, POLLIN, 0 }; poll(&pf, 1, 20); nf += cam_step(); }
            fprintf(stderr, "frames=%d in 10s (%.1f fps)\n", nf, nf / 10.0);
            cam_stop();
            goto write_dump;
        }
        cur = 6;
        for (int i = 0; i < MAXPAGE * NSLOT; i++) slots[i].st = !stateful(&slots[i]) ? 0 : (i % 3 == 0 ? 2 : 1);  // 見本用の状態
        slots[1].state = 2; slots[1].state_t = now();
        snprintf(status_msg, sizeof status_msg, "洗面 OK"); status_t = now();
        draw();
    write_dump:;
        FILE *f = fopen(dump, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { uint32_t p = bb[i]; fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f); }
        fclose(f);
        return 0;
    }

    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("fb0"); return 1; }
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
    W = vinfo.xres; H = vinfo.yres;
    fbmem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) { perror("mmap"); return 1; }
    bb = malloc(W * H * 4);

    long n;
    unsigned char *ft = load_file("/usr/trimui/res/full.ttf", &n);
    if (!ft) ft = load_file("/usr/trimui/res/regular.ttf", &n);
    char ip[300]; snprintf(ip, sizeof ip, "%s/mdi.ttf", appdir);
    unsigned char *fi = load_file(ip, &n);
    if (!ft || !fi) { fprintf(stderr, "font load failed\n"); return 1; }
    stbtt_InitFont(&font_txt, ft, stbtt_GetFontOffsetForIndex(ft, 0));
    stbtt_InitFont(&font_icon, fi, 0);

    load_config();
    set_screen(0);
    cam_buf = malloc(CAM_BUFSZ);

    int evfd = open(INPUT_DEV, O_RDONLY | O_NONBLOCK);
    if (evfd < 0) { perror(INPUT_DEV); return 1; }

    // 十字キーのリピート用
    int held = -1; double held_t = 0, rep_t = 0;
    int dirty = 1; double last_draw = 0;

    while (!quit_flag) {
        struct pollfd pf[2] = { { evfd, POLLIN, 0 }, { cam_fd, POLLIN, 0 } };
        poll(pf, cam_fd >= 0 ? 2 : 1, viewing >= 0 ? 15 : 50);
        struct input_event ev;
        while (read(evfd, &ev, sizeof ev) == sizeof ev) {
            int b = -1, down = 0;
            if (map_event(&ev, &b, &down)) {
                if (down) {
                    on_button(b, 0);
                    if (b >= BTN_L_UP && b <= BTN_L_RIGHT) { held = b; held_t = now(); rep_t = held_t; }
                } else if (b == held) held = -1;
                dirty = 1;
            }
        }
        double t = now();
        if (held >= 0 && t - held_t > 0.4 && t - rep_t > 0.12) { on_button(held, 1); rep_t = t; dirty = 1; }
        reap();
        // 状態取得（玄関アラートを見るため、カメラ表示中も続ける）
        if (poll_step()) dirty = 1;
        if (t >= next_poll && !screen_off) { poll_start(); next_poll = t + 3.0; }
        if (viewing >= 0) {   // カメラ表示中はグリッドを描かない
            if (!screen_off && cam_step()) present();
            if (viewing < 0) dirty = 1;
            continue;
        }
        static int alert_shown;
        if (alert_visible() != alert_shown) { alert_shown = !alert_shown; dirty = 1; }
        for (int i = pg * NSLOT; i < (pg + 1) * NSLOT; i++)
            if ((slots[i].state == 2 && t - slots[i].state_t < 1.0) || (slots[i].state == 3 && t - slots[i].state_t < 2.2)) dirty = 1;
        if (status_msg[0] && t - status_t < 4.2) dirty = 1;
        if (t - last_draw > 10) dirty = 1;  // 時計更新
        if (dirty && !screen_off) { draw(); present(); dirty = 0; last_draw = t; }
    }

    cam_stop();
    set_screen(0);
    memset(bb, 0, W * H * 4); present();
    return 0;
}
