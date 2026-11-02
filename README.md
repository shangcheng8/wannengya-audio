# 万能鸭录音机架

把指定 App 的声音低延迟送进 DAW，经过 VST3 处理后，再把宿主混音送回去，
作为 OBS / 抖音直播伴侣 / 微信可选的“麦克风”。

```
伴奏 App ──process tap──▶ 万能鸭 Agent ──共享内存──▶ Capture VST3
                                                        │
人声 ──声卡输入──────────────────▶ Studio One ◀─────────┘
                                        │
                         Return VST3 ──▶ Agent ──▶ wannengya 1/2 ──▶ OBS
```

## 这一版和上一版的区别

| | 上一版 | 这一版 |
|---|---|---|
| 虚拟声卡 | 8 个 bundle，一方向一个 | **1 个 bundle，4 个双向设备** |
| out→in 环回 | 每个方向一层 2048 帧缓冲，实测 44.7 ms | 按 sample time 寻址，**1～2 个 IO 周期** |
| 时钟漂移 | 无补偿，靠跳读指针，长跑周期性断音 | **每条链路一个 64-tap 窗函数 sinc 重采样器 + PI 控制** |
| App 进 DAW | 绕两趟虚拟设备，约 130 ms | **直连插件，约 10～15 ms** |
| Return 到 OBS | 约 97 ms | **约 10～15 ms** |
| 跨进程缓冲 | `/tmp` 文件，权限 0666 | **`shm_open` 0600，按 UID 命名** |
| 延迟上报 | 插件 0，驱动 1536 | 实测值 |
| 采样率 | 必须全链路同率 | App / 工程不同率自动重采样 |
| 后台存活 | 编辑器轮询拉起 | 心跳 + coreaudiod 重启监听 |
| 界面 | 文字说明堆叠 | 一行一个决定 |

## 目录

```
shared/   共享内存环形缓冲、漂移补偿、sinc 重采样、控制块
driver/   HAL 驱动：一个 bundle，四个双向设备
hub/      进程枚举、process tap、虚拟设备读写
agent/    后台服务：持有 TCC、跑 tap、播回传
plugins/  Capture / Return 两个 VST3
ui/       视觉规范、LookAndFeel、Logo
tools/    wngmon 诊断工具、界面离屏渲染
```

## 系统要求

- **macOS 12.0 及以上**，安装包与插件均为通用二进制（Apple Silicon + Intel 同一个包）。
- **应用采集（把单个 App 的声音送进 DAW）需要 macOS 14.2+**：它用的是 CoreAudio
  process tap，这个 API 从 14.2 才有。macOS 12 / 13 上插件里的应用下拉框会置灰并说明原因，
  虚拟通道与 Return 两条链路完全可用。
- 想在 12 / 13 上抓单个 App，只能让它输出到 wannengya 虚拟设备（App 自己支持选输出设备时才可行），
  或者另装 BlackHole / Loopback 这类虚拟声卡中转。

## 构建

需要 macOS 12 以上、Xcode 命令行工具、CMake 3.22 以上（`brew install cmake`）。

```sh
git clone https://github.com/shangcheng8/wannengya-audio.git
cd wannengya-audio
sh scripts/build.sh
```

JUCE 固定为 8.0.15：配置时如果 `third_party/JUCE` 不存在，CMake 会自动下载对应 tag
的源码包并校验 SHA-256；如果你已经放了一份（离线构建），直接放进 `third_party/JUCE`
即可，不会再下载。

`scripts/build.sh` 优先用 `third_party/cmake-3.31.6-macos-universal/`（如果你本地放了），
找不到就用 `PATH` 里的 `cmake`；想指定别的就 `CMAKE=/path/to/cmake sh scripts/build.sh`。

产物：

- `build/agent/wannengya Agent.app`
- `build/WannengyaCapture_artefacts/Release/VST3/wannengya Capture.vst3`
- `build/WannengyaReturn_artefacts/Release/VST3/wannengya Return.vst3`
- `build/drivers/Wannengya.driver`
- `build/wngmon`

两个 VST3 会自动复制到 `~/Library/Audio/Plug-Ins/VST3/`。

## 安装

```sh
sh scripts/install_driver.sh   # 需要管理员密码，会重启 coreaudiod
sh scripts/install_agent.sh
```

装好后 Audio MIDI Setup 里是四个**双向**设备：

| 设备 | 用途 |
|---|---|
| `wannengya 1/2` | DAW 混音 → OBS 麦克风（Return 默认） |
| `wannengya 3/4` | 外部播放器 → DAW（Capture 默认） |
| `wannengya 5/6` | 备用 |
| `wannengya 7/8` | 备用 |

纯虚拟通道路径不需要任何系统权限。只有“在插件里直接选 App”才需要
「屏幕与系统音频录制」，由 Agent 在首次使用时弹出。

## 使用

**Capture**：插在一条立体声 Bus 或总线轨上。

- 选 **App**：直接读取该 App 的声音（不需要把播放器输出改到虚拟设备）。
- 选 **虚拟通道**：读取任意一个 `wannengya X` 设备，外部播放器把输出设成同一个。
- 两者可以同时开，插件把两路相加，不做限幅（全程 float32）。

**Return**：插在 Studio One 的 Main 输出或 Stream 总线。

- 选 `wannengya 1/2` 后，OBS 麦克风选 `wannengya 1/2`。
- 回传通道和采集通道不要选同一对，插件会检测并拒绝。

## 延迟

一个 128 帧的 IO 周期在 48 kHz 是 2.67 ms。整条链路：

| 段 | 延迟 |
|---|---|
| 驱动 out→in | 1 个 IO 周期 + 64 帧 safety |
| Agent / 插件之间的 ring | 128 帧目标水位，由重采样器维持 |
| App tap | 由 macOS 决定，通常 1 个 IO 周期 |

Return 全链路 = 一个 ring 缓冲（128 帧）+ 一个设备 IO 周期 + 驱动环回：

| 组成 | 128 帧 / 48 kHz |
|---|---|
| Return 插件 → Agent ring | 2.7 ms（由重采样器稳住的 128 帧水位） |
| 虚拟设备输出缓冲 | 2.7 ms |
| 驱动 out→in | 2.7～5.3 ms |

合计 **约 8～11 ms**，设备缓冲调到 512 帧时约 14 ms。上一版同样的链路是 97 ms。
（新驱动需要安装后才能实测；上面的数字来自缓冲区配置，不是估算经验值。）

## 漂移补偿

DAW 的时钟（声卡晶振）和虚拟设备的时钟独立，实测相差约 5.5 ppm，
不处理的话每小时会有几次掉音。每个跨界链路都带一个 64-tap 窗函数 sinc
插值器，由 PI 控制器把缓冲水位稳在 128 帧：

- 比率恒为 1.0 时是逐样本拷贝（bit-exact）
- 补偿范围 ±600 ppm，远大于晶振误差
- 音高偏移最大 0.1 cent，人耳不可闻
- 实测 120 秒 × 5.5 / 50 ppm 正负两个方向，缓冲稳定在 129–156 帧，0 次失步

不同采样率的 App 或设备也走同一个插值器，不再直接报错。

## 测试

```sh
sh tests/run.sh
```

三组不需要安装任何东西的测试：

- **驱动环回**：直接调用驱动的 IO 路径，验证 1 个 IO 周期延迟、逐样本一致、
  未写入的设备返回静音、上报的 0 / 64 / 1024。
- **漂移长跑**：±5.5 / ±50 ppm 各跑 120 秒，验证缓冲稳定在目标水位附近。
- **块大小与采样率比**：128/256/512 帧，以及 44.1k↔48k、96k→48k。

## 排障：coreaudiod 占满 CPU、宿主打不开

如果重启宿主时 `coreaudiod` 冲到 80–100%，宿主卡在启动音频这一步，先查有没有重复的 HAL 驱动：

```sh
ls /Library/Audio/Plug-Ins/HAL/*.driver
grep -l E60A2918-B0D2-4B3D-9CD4-1697DE168E9E /Library/Audio/Plug-Ins/HAL/*.driver/Contents/Info.plist
```

第二个命令只应命中 `Wannengya.driver`。命中两个就说明有旧实验版本残留在同一目录，它们共用同一个 CFPlugIn factory UUID，所有音频客户端都会报 *More than one bundle with the same factory UUID*，`coreaudiod` 会在每次属性查询时重建插件表，CPU 直接打满。删掉多余的那个，然后：

```sh
sudo killall coreaudiod
```

1.0.6 起安装包会在 preinstall 里按 UUID 清理这类残留，所以正常升级不会再留下第二份。

## 诊断

```sh
build/wngmon          # 一次快照
build/wngmon -w       # 每 0.5 s 刷新
build/wngmon -r tap   # 单个 ring 的原始计数
```

权限引导窗口可以在进程内离屏渲染成 PNG，不需要系统的“屏幕录制”授权，
用它就能检查卡片里的文字有没有被摆到卡片外面：

```sh
"/Applications/wannengya Agent.app/Contents/MacOS/wannengya Agent" \
    --snapshot-permissions /tmp/guide.png
```

`wngreturnsim` 假装自己是 Return 插件：先心跳若干秒，再静默若干秒。用来确认
Agent 只在插件活着时打开虚拟输出，插件退出后不会每两秒反复启停：

```sh
build-release/wngreturnsim 6 6
```

Agent 是单实例的。安装器、插件、登录项可能在同一秒里各拉起一次，只有第一个
拿到锁的进程真正工作；其余进程把“显示授权引导”转给它，然后立刻退出，
不会有两个 worker 抢同一组 ring 和 tap。

## 已知限制

- 驱动只提供 2 声道设备；多声道虚拟设备还没有做。
- App 采集需要 macOS 14.2+（process tap）。
- 发布用的安装包目前是本机自签名 `MacLoopback Dev`，没有 Developer ID 公证，所以
  macOS 会要求用户手动放行；这不影响源码构建，只影响别人安装时的摩擦。
- JUCE 8 是 AGPLv3 / 商业双授权，本项目的许可选择见下一节。

## 许可

本项目以 **GNU Affero 通用公共许可证第 3 版（AGPL-3.0）** 发布，全文见
[LICENSE](LICENSE)。选它的原因是 JUCE 8 的双授权：AGPLv3 或 JUCE 商业许可。

- 你可以自由使用、修改、再分发，条件是按 AGPL-3.0 提供完整对应源码。
- 如果你想把本项目的代码并入闭源产品，需要自行获得
  [JUCE 商业许可](https://juce.com/legal/juce-8-licence)；AGPL-3.0 不允许那样做。
- 第三方组件（VST3 SDK、zlib、libpng、HarfBuzz、SheenBidi 等）各自的许可与声明见
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
- VST is a trademark of Steinberg Media Technologies GmbH。本项目与 Steinberg 无隶属关系。

AGPL-3.0 只授予版权许可，不授予商标许可，详见下面的「商标」。

## 隐私

- 本程序**不发起任何网络连接**，没有遥测、没有账号、没有云端处理。
- 采集到的音频只在进程内存和匿名共享内存里流转，不落盘；进程退出即释放。
- 需要「屏幕与系统音频录制」和「麦克风」两项系统权限，用途仅为读取你选中的 App
  的声音、以及 wannengya 虚拟输入通道；可以在系统设置里随时撤销。
- 卸载脚本会一并清除这些权限记录，见 [scripts/uninstall.sh](scripts/uninstall.sh)。

## 商标

「万能鸭」「wannengya」名称、`ui/logo.png` 图标以及 `com.wannengya.*` 这几个
bundle identifier 不在 AGPL-3.0 的授权范围内。fork 或再分发时请换成自己的名字和
bundle id，不要用它们发布你的修改版。

## 贡献

欢迎 issue 和 PR。提交代码即表示你同意以 AGPL-3.0 授权你的贡献，并且保留你对该贡献的
版权（不要求转让）。如果你希望本项目将来能提供商业许可，请不要提交你无法这样授权的代码。
