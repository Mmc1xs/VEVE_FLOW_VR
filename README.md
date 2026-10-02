# VEVE_FLOW_VR — 把 HTC VIVE Flow 當成 SteamVR 頭盔

PC 上的 SteamVR 畫面用 NVENC 編成 H.264，經 Wi-Fi 串流到 VIVE Flow；Flow 把頭部姿態回傳給 SteamVR。
主要用途：在 Flow 裡看 PC 桌面（Desktop+），並用一個 USB 數字小鍵盤當作 VR 控制器操作。

```
┌──────────────────────────── PC (Windows) ─────────────────────────────┐          ┌───── VIVE Flow ──────┐
│ SteamVR ── driver_flowvr.dll                                          │          │ Flow Probe APK       │
│             ├ HMD：投影/IPD = Flow 實測值，姿態來自 Flow (UDP 8002)   │◄─ UDP ───┤  ├ 送頭部姿態+序號   │
│             ├ 虛擬顯示：Present → GPU 縮放 → [編碼執行緒] NVENC ──────┼─ TCP ──► │  ├ MediaCodec 解碼   │
│             │   FLOWH264 v5，3200×1600 @75，每幀附「渲染用姿態序號」 │  8001    │  ├ 左右眼各取一半    │
│             │   沒有連線時在 UDP 8002 廣播 discovery                  │          │  └ 以渲染姿態提交給  │
│             └ 數字鍵盤控制器 (右手，雷射跟隨頭部) ◄─ UDP 127.0.0.1:8003│          │     Wave timewarp    │
│ flow_dashboard_helper.exe（SteamVR 自動啟動）                         │          └──────────────────────┘
│   ├ Flow 連上 / 遊戲結束時，若沒有遊戲在跑就打開 Desktop+ 分頁        │
│   └ 攔截數字鍵盤（SteamVR 執行中電腦收不到），轉送按鍵給驅動          │
│ Desktop+（Steam 免費工具）：「只在 Desktop+ 分頁」顯示主螢幕          │
└───────────────────────────────────────────────────────────────────────┘
```

## 目錄

| 路徑 | 內容 |
|---|---|
| `pc/flow_steamvr_driver/` | SteamVR 驅動（C++/CMake）。設定在 `flowvr/resources/settings/default.vrsettings` |
| `pc/flow_dashboard_helper/` | SteamVR 背景程式：自動開 Desktop+ 分頁、數字鍵盤擷取 |
| `Wave_Native_SDK/samples/wvr_flow_probe/` | Flow 端 APK（Wave Native SDK，Java + C++/GLES） |
| `Wave_Native_SDK/repo/` | Wave SDK 本機 Maven 套件（**不在版控內**，需自行下載，見下方「Wave SDK」） |
| `pc/openvr/` | OpenVR SDK v2.15.6（git submodule） |
| `pc/third_party/nv-codec-headers/` | NVENC API 標頭 |
| `scripts/` | `build.ps1`、`install.ps1`、`uninstall.ps1` |
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
4. 開 VR 遊戲時桌面自動隱藏；遊戲結束（回到 Home）後自動再打開。拿下頭盔再戴上也會重新打開。

### 數字鍵盤（只在 SteamVR 執行中有效，NumLock 開關不影響）

| 鍵 | VR 控制器 | 用途 |
|---|---|---|
| 5 | Trigger | 選取/點擊（按住＝拖曳） |
| 0 / Ins | Grip | 返回 |
| Enter | 觸控板按下 | — |
| + / 8，− / 2 | 觸控板上 / 下 | 捲動 |
| 4 / 6 | 觸控板左 / 右 | 方向 |
| * | System | 開關 SteamVR 控制台 |
| / | Menu | 遊戲選單 |

用頭部對準（雷射跟著頭），按鍵點擊。SteamVR 執行期間小鍵盤不會打字到電腦；NumLock 與小鍵盤的 ←（Backspace）照常。

## 設定在哪裡

| 設定 | 位置 | 套用方式 |
|---|---|---|
| 解析度 3200×1600、位元率 100 Mbps、NVENC P4、FOV、IPD | `pc/flow_steamvr_driver/flowvr/resources/settings/default.vrsettings` | `build.ps1`（複製到 dist）後重開 SteamVR |
| 數字鍵盤控制器開關 | 同上 `driver_flowvr.enable_keypad_controller` | 同上 |
| Desktop+ 大小 248 cm、下移 22 cm、只在 Desktop+ 分頁 | `scripts/install.ps1` 開頭 `$DesktopPlusOverlay` | `install.ps1` |
| SteamVR overlay 品質 High | `scripts/install.ps1` 開頭 `$SteamVRSettings` | `install.ps1` |
| Flow 眼睛緩衝 1600、銳化（預設關） | `.../wvr_flow_probe/app/src/main/jni/hellovr.cpp` 開頭的 `FLOW_*` | `build.ps1` + `install.ps1` |

網路：TCP 8001（影像 PC→Flow）、UDP 8002（姿態 Flow→PC、discovery PC→Flow）、UDP 127.0.0.1:8003（鍵盤→驅動）。
Windows 防火牆若詢問，請允許 SteamVR (vrserver) 使用私人網路。

## 疑難排解

- **只看到 SteamVR Home、沒有桌面**：拿下頭盔再戴上（Flow 重新連線會再打開）。若仍沒有，按 `*` 開控制台，切到 Desktop+ 分頁。
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

## 已知限制

- Flow 面板每度像素少於桌面：小字偏軟，主要靠放大 Desktop+ 畫面改善（目前 248 cm）。
- 往返延遲約 55 ms；頭部轉動由 Wave timewarp 依渲染姿態補償，平移不補償。
- 小鍵盤的 ←（Backspace）與主鍵盤無法區分，所以不攔截。
