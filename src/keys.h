// TRIMUI BRICK Pro のボタン割り当て（/dev/input/event3 "TRIMUI Player1"）
// 2026-11-16 のキーテストで確認: 十字キー=ABS_HAT0X/HAT0Y、A=305 B=304 L1=310 R1=311
// SELECT=314 START=315 MENU=316。X=307 Y=308 は Linux の標準割り当てからの推定
#define INPUT_DEV "/dev/input/event3"

enum { BTN_L_UP, BTN_L_DOWN, BTN_L_LEFT, BTN_L_RIGHT,
       BTN_L_A, BTN_L_B, BTN_L_X, BTN_L_Y, BTN_L_L1, BTN_L_R1,
       BTN_L_SELECT, BTN_L_START, BTN_L_MENU };

static int hat_x, hat_y;

// evdev のイベント → 論理ボタン。対象なら 1 を返す
static int map_event(const struct input_event *ev, int *b, int *down) {
    if (ev->type == EV_KEY) {
        if (ev->value == 2) return 0;  // カーネルのリピートは使わない
        *down = ev->value;
        switch (ev->code) {
        case 305: *b = BTN_L_A; return 1;
        case 304: *b = BTN_L_B; return 1;
        case 307: *b = BTN_L_X; return 1;
        case 308: *b = BTN_L_Y; return 1;
        case 310: *b = BTN_L_L1; return 1;
        case 311: *b = BTN_L_R1; return 1;
        case 314: *b = BTN_L_SELECT; return 1;
        case 315: *b = BTN_L_START; return 1;
        case 316: *b = BTN_L_MENU; return 1;
        }
        return 0;
    }
    if (ev->type == EV_ABS && ev->code == ABS_HAT0X) {
        int v = ev->value;
        if (v) { *b = v < 0 ? BTN_L_LEFT : BTN_L_RIGHT; *down = 1; }
        else { *b = hat_x < 0 ? BTN_L_LEFT : BTN_L_RIGHT; *down = 0; }
        hat_x = v; return 1;
    }
    if (ev->type == EV_ABS && ev->code == ABS_HAT0Y) {
        int v = ev->value;
        if (v) { *b = v < 0 ? BTN_L_UP : BTN_L_DOWN; *down = 1; }
        else { *b = hat_y < 0 ? BTN_L_UP : BTN_L_DOWN; *down = 0; }
        hat_y = v; return 1;
    }
    return 0;
}
