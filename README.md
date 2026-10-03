# VEVE_FLOW_VR — 把 HTC VIVE Flow 當成 SteamVR 頭盔

PC 上的 SteamVR 畫面用 NVENC 編成 H.264，經 Wi-Fi 串流到 VIVE Flow；Flow 把頭部姿態回傳給 SteamVR。
主要用途：在 Flow 裡看 PC 桌面（Desktop+）；Flow 內建的手部追蹤當作兩支 Index 控制器，USB 數字小鍵盤補上搖桿與按鍵
（手不在視野內時，小鍵盤是跟著頭部的雷射控制器）。

```
┌──────────────────────────── PC (Windows) ─────────────────────────────┐          ┌───── VIVE Flow ──────┐
│ SteamVR ── driver_flowvr.dll                                          │          │ Flow Probe APK       │
│             ├ HMD：投影/IPD = Flow 實測值，姿態來自 Flow (UDP 8002)   │◄─ UDP ───┤  ├ 送頭部姿態+序號   │
│             │   追蹤空間 "FLOW"；Flow 有送姿態 = 使用者戴著           │          │  ├ 送雙手關節+捏合   │
│             ├ 虛擬顯示：Present → GPU 縮放 → [編碼執行緒] NVENC ──────┼─ TCP ──► │  ├ MediaCodec 解碼   │
│             │   FLOWH264 v5，3200×1600 @75，每幀附「渲染用姿態序號」 │  8001    │  ├ 左右眼各取一半    │
│             │   沒有連線時在 UDP 8002 廣播 discovery                  │          │  └ 以渲染姿態提交給  │
│             ├ 數字鍵盤控制器 (右手，雷射跟隨頭部) ◄─ UDP 127.0.0.1:8003│          │     Wave timewarp    │
│             └ Index 控制器 ×2：Flow 手部追蹤；右手兼收數字鍵盤        │          │                      │
│ flow_dashboard_helper.exe（SteamVR 自動啟動）                         │          └──────────────────────┘
│   ├ Flow 連上 / 遊戲結束時，若沒有遊戲在跑就打開 Desktop+ 分頁        │
│   ├ 遊戲開始時關閉控制台；坐姿原點未設定時以目前頭部位置設定          │
│   └ 攔截數字鍵盤（SteamVR 執行中電腦收不到），轉送按鍵給驅動          │
│ Desktop+（Steam 免費工具）：「只在 Desktop+ 分頁」顯示主螢幕          │
└───────────────────────────────────────────────────────────────────────┘
```

## 目錄

| 路徑 | 內容 |
|---|---|
| `pc/flow_steamvr_driver/` | SteamVR 驅動（C++/CMake）。設定在 `flowvr/resources/settings/default.vrsettings` |
| `pc/flow_dashboard_helper/` | SteamVR 背景程式：自動開/關控制台、數字鍵盤擷取 |
| `Wave_Native_SDK/samples/wvr_flow_probe/` | Flow 端 APK（Wave Native SDK，Java + C++/GLES） |
| `Wave_Native_SDK/repo/` | Wave SDK 本機 Maven 套件（**不在版控內**，需自行下載，見下方「Wave SDK」） |
| `pc/openvr/` | OpenVR SDK v2.15.6（git submodule） |
| `pc/third_party/nv-codec-headers/` | NVENC API 標頭 |
| `scripts/` | `build.ps1`、`install.ps1`、`uninstall.ps1`、`dev-awake.ps1`（開發模式） |
| `pc/flow_desktop_streamer/`、`.../wvr_flow_probe/tools/` | 備用模式：不經 SteamVR 直接串流桌面（Python + ffmpeg） |
| `flow_probe/` | 從 Flow 擷取的硬體資訊（解碼器能力、顯示器等），做為參考；含裝置序號的 `getprop.txt` 不公開 |

## 建置環境

- Windows 10/11、NVIDIA GPU（NVENC）、Steam + SteamVR、Desktop+（Steam 免費，app 1494460）
- Visual Studio 2022（「使用 C++ 的桌面開發」）、CMake ≥ 3.15
- Android SDK（Android Studio）＋ **NDK 21.4.7075529**；**JDK 8**（需完整 JDK，不是 JRE）
  - `JAVA_HOME` 指向 JDK 8，或把 Temurin JDK 8 解壓到 `tools\jdk8\<資料夾>`（`tools/` 不進版控）
  - 第一次建置 APK 需要網路（Gradle 5.6.1 / AGP 3.5 會下載依賴）
- adb（Android SDK platform-tools），Flow 開啟 USB 偵錯
- 選用：.NET 9 SDK、Python 3 + ffmpeg（只有備用直接串流模式需要）

## Wave SDK

HTC Wave SDK 依「VIVE SDK License Agreement」授權，不包含在這個倉庫裡，請自行下載：

1. 到 VIVE 開發者網站（https://developer.vive.com ，需登入）下載 **Wave Native SDK 4.5.0**。
2. 把壓縮檔裡的 `repo` 資料夾整個複製到本專案的 `Wave_Native_SDK\repo`，
   確認存在 `Wave_Native_SDK\repo\com\htc\vr\wvr_client\4.5.0-u02\wvr_client-4.5.0-u02.aar`。

Flow App（`wvr_flow_probe`）最初改寫自 Wave SDK 的 hello-VR 範例；沒有用到的範例檔案（3D 場景、貼圖、shader）已移除。

## 第一次安裝

```powershell
git clone <repo> VEVE_FLOW_VR
cd VEVE_FLOW_VR
git submodule update --init          # 取得 pc/openvr (v2.15.6)
```
0. 依上一節放好 Wave SDK。
1. 從 Steam 安裝 **Desktop+**，**先啟動一次**（會帶起 SteamVR，並自動設定隨 SteamVR 啟動），再關閉 SteamVR。
2. 建置（SteamVR 必須關閉）：`powershell -ExecutionPolicy Bypass -File scripts\build.ps1`
3. Flow 用 USB 接上電腦後安裝/套用設定：`powershell -ExecutionPolicy Bypass -File scripts\install.ps1`

`install.ps1` 可以重複執行：註冊驅動、設定 SteamVR、套用 Desktop+ 設定、註冊背景程式、安裝 APK。
第一次修改設定檔前會留 `*.bak-veve` 備份；`scripts\uninstall.ps1 [-RestoreSettings] [-RemoveApk]` 可以還原。

## 日常使用

1. Flow 與 PC 在同一個 Wi-Fi（不需要 USB / ADB）。
2. 從 Steam 啟動 SteamVR（會一併開啟 SteamVR Home、Desktop+、背景程式）。
3. 在 Flow 上開啟 **Flow Probe**。連上後約 1.5 秒，Desktop+ 分頁自動打開，看到 PC 主螢幕。
4. 開 VR 遊戲時控制台（連同桌面）自動關閉；遊戲結束（回到 Home）後自動再打開。拿下頭盔再戴上也會重新打開。
5. 雷射點到 Desktop+ 面板外會關掉控制台、桌面跟著消失：按 `*` 叫回來（SteamVR 會打開上次用的 Desktop+ 分頁）。
6. 雙手舉到眼前就變成兩支 Index 控制器（見下方「手部追蹤」）；手放下約 3 秒後，小鍵盤回到跟著頭部的雷射。

### 手部追蹤（Index 控制器）

Flow 的鏡頭追蹤雙手（每手 26 個關節），驅動把它們變成 SteamVR 的 Valve Index 控制器（左右各一）。

| 手勢 / 按鍵 | Index 控制器 |
|---|---|
| 拇指捏食指 | Trigger（類比；捏緊＝點擊） |
| 握拳（中指、無名指、小指彎曲） | Grip |
| 各手指彎曲 | 手指彎曲量（`/input/finger/*`） |
| 右手被追蹤時的小鍵盤：方向鍵 / + − | 右手搖桿 |
| 　Enter / `/` / `*` | A / B / System |
| 　5 / 0 | Trigger / Grip（與手勢合併） |

- 控制器位置在手掌中心，方向由手腕→中指根部與食指↔小指根部算出；雷射方向用 `hand_pitch_offset_deg` 微調。
- 手離開鏡頭視野（例如放到身側）就失去追蹤；連續 3 秒沒追蹤到才斷線，避免短暫遺失時跳動。
- 握拳時捏合強度也會升高，所以會同時觸發 Trigger（類似用力握實體 Index）。

### 數字鍵盤（只在 SteamVR 執行中有效，NumLock 開關不影響）

右手沒有被追蹤時，小鍵盤是獨立的右手控制器：

| 鍵 | VR 控制器 | 用途 |
|---|---|---|
| 5 | Trigger | 選取/點擊（按住＝拖曳） |
| 0 / Ins | Grip | 返回 |
| Enter | 觸控板按下 | — |
| + / 8，− / 2 | 觸控板上 / 下 | 捲動 |
| 4 / 6 | 觸控板左 / 右 | 方向 |
| * | System | 開關 SteamVR 控制台（叫回 Desktop+） |
| / | Menu | 遊戲選單 |

用頭部對準（雷射跟著頭），按鍵點擊。SteamVR 執行期間小鍵盤不會打字到電腦；NumLock 與小鍵盤的 ←（Backspace）照常。

## 設定在哪裡

| 設定 | 位置 | 套用方式 |
|---|---|---|
| 解析度 3200×1600、位元率 100 Mbps、NVENC P4、FOV、IPD | `pc/flow_steamvr_driver/flowvr/resources/settings/default.vrsettings` | `build.ps1`（複製到 dist）後重開 SteamVR |
| 數字鍵盤控制器開關 | 同上 `driver_flowvr.enable_keypad_controller` | 同上 |
| 手部 Index 控制器開關、雷射俯仰微調 | 同上 `driver_flowvr.enable_hand_controllers`、`hand_pitch_offset_deg` | 同上 |
| Flow 手部追蹤（預設開） | `hellovr.cpp` 的 `FLOW_DEFAULT_HANDS`；即時開關 `adb shell setprop debug.flow.hands 0/1` | APK 或 setprop |
| Desktop+ 大小 248 cm、下移 22 cm、只在 Desktop+ 分頁 | `scripts/install.ps1` 開頭 `$DesktopPlusOverlay` | `install.ps1` |
| SteamVR overlay 品質 High、閒置 10 分鐘進入待機 | `scripts/install.ps1` 開頭 `$SteamVRSettings` | `install.ps1`（存在 `steamvr.vrsettings`，重開機仍有效） |
| Flow 眼睛緩衝 1600、銳化（預設關） | `.../wvr_flow_probe/app/src/main/jni/hellovr.cpp` 開頭的 `FLOW_*` | `build.ps1` + `install.ps1` |

網路：TCP 8001（影像 PC→Flow）、UDP 8002（頭部姿態與雙手 Flow→PC、discovery PC→Flow）、UDP 127.0.0.1:8003（鍵盤→驅動）。
Windows 防火牆若詢問，請允許 SteamVR (vrserver) 使用私人網路。

## 開發模式（不戴頭盔測試）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev-awake.ps1        # 開啟
powershell -ExecutionPolicy Bypass -File scripts\dev-awake.ps1 -Off   # 關閉
```

- **Flow 不休眠**：Flow 的 OEM 服務在距離感測器判斷「沒戴」約 5 秒後強制休眠，Android 的螢幕設定蓋不過它。
  腳本等你遮住鼻樑內側的感測器（或戴上），偵測到「已戴上」後用 `dumpsys sensorservice restrict` 凍結感測器事件，
  之後拿下也維持清醒。頭部追蹤不受影響。需要 ADB；Flow 重開機即失效。
- **SteamVR 閒置 30 分鐘才待機**（平常 10 分鐘）：頭盔放在桌上不動會被視為閒置，待機時畫面全黑。
  SteamVR 執行中透過背景程式的 `--idle-timeout` 修改，否則直接改 `steamvr.vrsettings`；`-Off` 改回 10 分鐘。

## 疑難排解

- **進 VR 遊戲畫面整片灰色（#4F5A64），但電腦上的遊戲視窗正常**：SteamVR 判定追蹤失效（`trackingLossColor`）。
  坐姿模式的遊戲（Unity 預設）需要追蹤空間的坐姿原點；驅動設定追蹤空間 "FLOW"，背景程式在 Flow 連上時補設坐姿原點。
  若仍發生，看背景程式記錄是否有 `seated zero pose`，或在 SteamVR 選單「重置坐姿位置」。
- **遊戲畫面變暗、解析度變低、不會動**：控制台還開著（舊版 SteamVR Unity 外掛在沒有輸入焦點時會暫停）。按 `*` 關閉。
- **只看到 SteamVR Home、沒有桌面**：按 `*` 打開控制台；或拿下頭盔再戴上（Flow 重新連線會再打開）。
- **畫面中央「選擇 USB 模式」**：Flow 接著 USB 時的系統提示，選「不執行任何動作」，或拔掉 USB。
- **「無法追蹤頭戴式裝置」**：環境太暗或鏡頭被擋住。
- **Desktop+ 設定被改回去**：Desktop+ 關閉時會寫回自己的設定；改設定前先關閉 SteamVR，或直接重跑 `install.ps1`。
- **建置驅動失敗（檔案被鎖定）**：先關閉 SteamVR。

### 診斷工具

- 驅動記錄：`pc\flow_steamvr_driver\build\dist\flowvr\logs\flow_virtual_display_trace.log`（每 2 秒一行 `stream stats`：SteamVR Present 頻率、編碼時間、送出幀率）
- SteamVR 實際輸出畫面：在 `...\dist\flowvr\logs\` 建立空檔 `dump_preview.request`，約 1 秒內產生 `flow_compositor_preview.ppm`
- Flow 畫面：`adb exec-out screencap -p > flow.png`（很暗，需要調整對比）
- Flow 記錄：`adb logcat -s FlowProbe vrsample`（`stream rates` 收/解碼幀率、`timewarp poseAge` 往返延遲，1 步 ≈ 13.3 ms）
- 背景程式記錄：`pc\flow_dashboard_helper\build\dist\flow_dashboard_helper.log`
- 銳化即時調整：`adb shell setprop debug.flow.sharpen 0.8`（0–2，0 = 關）
- 手部：SteamVR 記錄 `Steam\logs\vrserver.txt` 每 2 秒一行 `Flow hand ...`（捏合、Trigger、各指彎曲、Grip、鍵盤）；Flow 記錄的 `hands` 行（追蹤頻率、左右手有效比例、捏合比例）

## 已知限制

- Flow 面板每度像素少於桌面：小字偏軟，主要靠放大 Desktop+ 畫面改善（目前 248 cm）。
- 往返延遲約 55 ms；頭部轉動由 Wave timewarp 依渲染姿態補償，平移不補償。
- 小鍵盤的 ←（Backspace）與主鍵盤無法區分，所以不攔截。
- 手部控制器還沒有手指骨架（`/input/skeleton`）：遊戲裡看到的是 Index 控制器模型，不會顯示手指動作。
- 搖桿只能用小鍵盤，手勢沒有對應。

## 待辦

- 手指骨架：把 Flow 的 26 個關節轉成 OpenVR 手部骨架，支援 Index 手指追蹤的遊戲（Half-Life: Alyx、VRChat 等）就能顯示手指。
- 長時間開手部追蹤時 Flow 的溫度與降頻（目前只測過幾分鐘）。
