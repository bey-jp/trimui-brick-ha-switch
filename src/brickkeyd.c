/*
 * brickkeyd: L2 + R2 を 1 秒同時押しすると MainUI を再起動する常駐プログラム。
 * MainUI は runtrimui.sh のループで自動的に起動し直し、アプリ一覧を読み直す。
 * アプリやゲームの実行中は MainUI が動いていないので何もしない。
 * 起動: /etc/init.d/brickkeyd（S98）
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEV        "/dev/input/event3"   /* TRIMUI Player1 */
#define HOLD_MS    1000
#define TRIG_ON    128                   /* L2/R2 は 0..255 */

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* comm が MainUI のプロセスを探す。見つからなければ 0 */
static pid_t find_mainui(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char path[64], comm[32];
        pid_t pid = atoi(e->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f) && strcmp(comm, "MainUI\n") == 0) found = pid;
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

int main(void) {
    for (;;) {
        int fd = open(DEV, O_RDONLY);
        if (fd < 0) { sleep(2); continue; }
        int l2 = 0, r2 = 0, fired = 0;
        long since = 0;
        for (;;) {
            struct pollfd p = { fd, POLLIN, 0 };
            int both = l2 >= TRIG_ON && r2 >= TRIG_ON;
            int r = poll(&p, 1, both && !fired ? 50 : -1);
            if (r < 0) break;
            if (r > 0) {
                struct input_event ev[32];
                ssize_t n = read(fd, ev, sizeof ev);
                if (n <= 0) break;
                for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
                    if (ev[i].type != EV_ABS) continue;
                    if (ev[i].code == ABS_Z)  l2 = ev[i].value;
                    if (ev[i].code == ABS_RZ) r2 = ev[i].value;
                }
            }
            int now_both = l2 >= TRIG_ON && r2 >= TRIG_ON;
            if (!now_both) { since = 0; fired = 0; continue; }
            if (!since) since = now_ms();
            if (!fired && now_ms() - since >= HOLD_MS) {
                fired = 1;              /* 離すまで再実行しない */
                pid_t pid = find_mainui();
                if (pid) kill(pid, SIGKILL);
            }
        }
        close(fd);
        sleep(1);
    }
}
