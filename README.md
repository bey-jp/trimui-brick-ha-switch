# TRIMUI BRICK Pro HA Switch

携帯ゲーム機 **TRIMUI BRICK Pro** を Home Assistant のリモコンにするアプリです。

- 4段×5列 = 20 個のアイコン付きスイッチを十字キーで選び、A ボタンで ON/OFF
- 各スイッチの状態を 3 秒ごとに取得して色で表示（ON=黄色、OFF=灰色）
- 最大 6 ページ。L / R でページ切り替え
- go2rtc のカメラを全画面表示（640x360 で 7〜10fps）
- HA のタイマーが動いている間、画面の縁を赤く点滅（来客通知などに）
- 標準ファームのまま動作（カスタムファーム不要）。アプリフォルダを置くだけ

検証環境: TRIMUI BRICK Pro 標準ファーム（UI バージョン 20260828）、Home Assistant 2026.11

## 仕組み

```
[BRICK Pro: HA Switch]
   ├─ A ボタン → HA の Webhook に POST {"entity":"light.xxx"} → HA の自動化が操作
   ├─ 3 秒ごと → HA の REST API /api/template で状態をまとめて取得（長期アクセストークン）
   └─ カメラページ → go2rtc の MJPEG を受信して表示
```

アプリは C で書かれていて、画面（/dev/fb0）に直接描画し、ゲームパッド（/dev/input/event3）を読みます。
通信には本体に入っている curl を使います。

## インストール

1. `app/HASwitch/` フォルダを Brick の **`/mnt/UDISK/Apps/HASwitch/`** にコピーします
   （SD カードの `Apps/` でも動きますが、本体内部に置くと SD を替えても残ります）。
   - SSH を使う場合: 設定 → デベロッパー で SSH をオン（ユーザー root）。**初期パスワードは変更してください。**
2. `config.txt` を自分の環境に合わせて編集します（下記）。
3. HA の長期アクセストークンを作り（HA のプロフィール → セキュリティ → 長期アクセストークン）、
   文字列だけを `token` というファイル名で同じフォルダに置きます。トークンが無くても操作はできますが、状態の色表示が出ません。
4. アプリ一覧に「HA Switch」が出ます。出ないときは本体を再起動してください。

## Home Assistant 側の設定

1. `homeassistant/automation_webhook.yaml` の自動化を `automations.yaml` に追加します。
   - scene / script は実行、lock は施錠⇔解錠、それ以外は toggle します。
   - 受け付けるドメイン: light / switch / scene / script / lock / input_boolean / fan
2. カメラを使う場合は `homeassistant/go2rtc_example.yaml` を参考に、go2rtc に MJPEG のストリームを追加します
   （Brick は H.264 を表示できないため）。
3. 赤い点滅を使う場合は、タイマーのヘルパー（例 `timer.genkan_alert`、30 秒）を作り、
   人感センサーやドアベルの自動化で `timer.start` します。`config.txt` に `alert=timer.genkan_alert` と書きます。

## config.txt

```
url=http://192.168.0.10:8123/api/webhook/brick_switch
camurl=http://192.168.0.10:1984
alert=timer.genkan_alert
xbutton=script.all_toggle

[照明]
リビング|ceiling-light|light.living
全照明|lightbulb-group|script.all_toggle|input_boolean.all_power
-
カメラ|cctv|cam:front_door

[玄関]
camera=front_door
```

| 書き方 | 意味 |
|---|---|
| `url=` | HA の Webhook の URL |
| `camurl=` | go2rtc の URL |
| `alert=` | このタイマーが動いている間、画面の縁が赤く点滅（省略すると点滅なし） |
| `xbutton=` | X ボタンで実行する entity（スロットのどこかに同じ entity が必要） |
| `表示名\|アイコン\|entity_id` | スイッチ 1 個。左上から右へ並ぶ |
| 4 列目に entity | 状態表示だけ別の entity を見る |
| `-` | 空きマス |
| `[ページ名]` | 新しいページ（1 ページ 20 個、最大 6 ページ） |
| `[ページ名]` の次に `camera=<名前>` | そのページはカメラの全画面表示 |
| 3 列目が `cam:<名前>` | A でそのカメラのページへ移動 |

アイコンは [Material Design Icons](https://pictogrammers.com/library/mdi/) の名前（`mdi-` は不要）です。
編集したら Brick で **START** を押すと読み直します。

## ボタン

| ボタン | 動作 |
|---|---|
| 十字キー | 選択の移動（押し続けるとリピート） |
| A | 実行 |
| Y | 直前に実行したスイッチをもう一度 |
| X | `xbutton=` の entity を実行 |
| L / R | 前 / 次のページ |
| SELECT | 画面 OFF（どれかのボタンで点灯） |
| START | config.txt の読み直し |
| B / MENU | 終了 |

## おまけ: brickkeyd（L2+R2 でアプリ一覧を読み直す）

SD カードを抜き差しすると、本体内部に置いたアプリが一覧から消えることがあります。
`system/brickkeyd` を常駐させると、L2+R2 を 1 秒同時押しで MainUI を再起動して一覧を読み直せます。

```sh
scp system/brickkeyd root@<BrickのIP>:/usr/bin/
scp system/brickkeyd.init root@<BrickのIP>:/etc/init.d/brickkeyd
ssh root@<BrickのIP> 'chmod +x /usr/bin/brickkeyd /etc/init.d/brickkeyd && /etc/init.d/brickkeyd enable && /etc/init.d/brickkeyd start'
```

## ビルド

aarch64 の Linux（Raspberry Pi 5 など）で `sh src/build.sh`。Brick の glibc が古いため静的リンクでビルドします。
`app/HASwitch/haswitch` と `system/brickkeyd` が作られます。

## 注意

- Webhook は認証なしです（`local_only: true` で LAN 内からのみ）。LAN 内の誰でも操作でき、鍵の解錠もできます。
  気になる場合は、自動化の条件を entity の許可リストにしてください。
- 自己責任でご利用ください。

## ライセンス

- このリポジトリのコード: MIT License（`LICENSE`）
- `src/third_party/stb_truetype.h`, `stb_image.h`: Sean Barrett、パブリックドメイン / MIT
- `app/HASwitch/mdi.ttf`, `src/third_party/mdi.css`（@mdi/font 7.4.47）: Pictogrammers（https://pictogrammers.com/docs/general/license/）

紹介記事: https://bey.jp/
