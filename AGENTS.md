# Trophy Bootstrap Agent 指南

初始化与文档日期：2026-10-11。版本：0.1.0。默认中文沟通，用户当前明确指令优先。

## 项目范围

本项目是运行于 PS Vita SceShell `*main` 的常驻奖杯预热插件。宿主提交 Title ID / NPWR 队列，插件依次启动已安装游戏，由游戏自身初始化个人奖杯 DAT，再结束本轮认领的目标进程并检查配对。

只维护本项目源码、控制工具和测试。插件不安装游戏，不解锁奖杯，不修改 DAT，不删除奖杯数据库，不自动释放生成记录。没有独立气泡、设备按键入口或开机自动执行功能。

首先确认工作目录、Git 根与既存 diff，再根据任务阅读涉及源码。保留既存修改，不自动提交、推送或发布；用户已经明确授权的操作继续完成。

## 设备操作约束

文档阅读和整理不授权连接设备、部署、修改配置或执行队列。操作已获授权时，先核对当次设备地址、挂载、源和目标，不沿用历史环境。

部署前读取实际 tai 配置，保存配套备份，并核对候选 diff。在现有 `*main` 段增加 `ur0:tai/trophy_bootstrap.suprx`；已有配置无需重复添加。不删除其他插件行，不替换整个 tai 目录。加载插件需要设备重启，按授权范围执行。

默认待命，重启不自动恢复旧批次。权限、身份、存储、日志或结束进程错误时停止并保留现场；恢复必须使用当前会话，不能继承旧 PID 归属。禁止重写活动批次的不可变队列或伪造 journal。

DAT 配对存在只证明观察到两个非空文件，不能代替明文结构、零进度或获杯资格验证。批次终态必须逐项检查，不以退出码或 error=0 代替验收。

## 源码职责

| 文件 | 职责 |
| --- | --- |
| [bootstrap.c](src/bootstrap.c)、[bootstrap.h](include/bootstrap.h) | 队列解析、状态机、容量、归属、暂停/恢复及结果码 |
| [plugin.c](src/plugin.c) | SceShell 驻留线程、AppMgr 启停、目录查询、控制文件及日志 |
| [app_list.c](src/app_list.c) | 从小容量起逐级请求完整运行应用列表，拒绝截断和契约不符 |
| [app_status.c](src/app_status.c) | 区分启动上下文、已运行游戏和已知系统后台服务 |
| [sha256.c](src/sha256.c) | 队列及 journal 摘要 |
| [control.py](tools/control.py) | 电脑本地准备队列、发布控制文件、读取状态；不含 FTP |
| [build.sh](tools/build.sh)、[verify_plugin.py](tools/verify_plugin.py) | 固定 SDK 构建及 SELF 外层检查 |

原游戏自己调用奖杯初始化。插件不替游戏创建奖杯上下文，不绕过通信签名，不替换 eboot。每项仅提供 Title ID 和 NPWR；等待、停止和结果检查逻辑共用。

## 状态机

```text
PREFLIGHT → LAUNCH → WAIT_PROCESS → RUN_WINDOW
→ STOP → WAIT_EXIT → CHECK_PAIR → COOLDOWN → 下一项
```

| 环节 | 条件与上限 |
| --- | --- |
| PREFLIGHT/LAUNCH | 已有配对跳过；其他应用存在时等待；核对安装资源及实时容量；LAUNCH journal 先落盘 |
| WAIT_PROCESS | started 非零、application_type=1、名字/PID/App ID 与 Shell 列表对应；同一 PID 稳定 1 秒；启动等待最多 30 秒 |
| RUN_WINDOW | 保持已认领 PID；配对稳定 500 毫秒且运行满 5 秒即可停止，否则到 10/30 秒队列上限；PID 更换不重绑 |
| STOP | 对目标 PID 与 App ID 再次核对，STOP journal 先落盘，再结束本轮拥有的目标 |
| WAIT_EXIT | 最多 10 秒确认退出；未知或变化的 PID 不能当作退出 |
| CHECK_PAIR | 等 1.5 秒，核对两份 DAT 存在且非空，记录逐项结果 |
| COOLDOWN | 等 1.5 秒，进入下一项 |
| RECOVER_WAIT | 不继承旧 PID 归属；目标必须缺席，观察 3 秒后检查已出现配对，再跳过或继续 |

`process=0` 表示缺席；`1` 表示可认领的运行游戏；`2` 表示存在但启动中，不能当缺席或认领。负数错误不能按“没有进程”处理。实际延迟还受 Shell 调度影响，日志未逐阶段记录完整时间戳，不据此报告精确生成秒数。

## 容量及外部应用

最大 100 个队列项、100 组个人登记目录。容量检查按实际个人目录计数，同款 app/patch/rePatch 的可见多个 NPWR 预留多个名额。查询失败不视为空目录；资源/SFO 冲突不自行选一个覆盖。其他组件并发注册组的竞争尚未完整验证。

应用列表从 1 项开始，满额时扩大到 2/4/8/16/32；更大请求失败不能回退采用此前部分列表。最大容量仍满、返回值和输出不一致均报错。

插件对 `NPXS10063`、`NPXS10079` 仅在完整名字/PID/App ID 核对通过且 type=0 时视为后台服务，不等待或结束它们。其余系统应用及 VitaShell 仍阻挡。PREFLIGHT 中普通占用为等待；RUN 中遇到外部应用时先停止拥有的目标、检查结果，然后暂停整批。启动确认提示可能使 WAIT_PROCESS 超时；已有 DAT 不表示已运行到主菜单或具备正常获杯资格。

## 队列与控制

队列最多 8191 字节、LF 换行，不接受 NUL、CR、重复 Title ID 或重复 NPWR。窗口仅接受下面两种之一；Title ID 为九位大写字母/数字且不能以 NPXS 开头。以下标识为格式占位符。

```text
TROPHY_BOOTSTRAP_QUEUE_V1
batch=0123456789abcdef0123456789abcdef
user=00
window_ms=5000,30000
PCSE00001 NPWR00001_00
```

另一窗口为 `window_ms=5000,10000`。新批次使用新 batch 标识。运行和恢复采用 `jobs/<batch>/queue.txt` 不可变副本，不采用后来替换的根 queue.txt。

```text
TROPHY_BOOTSTRAP_COMMAND_V1
<当前32位十六进制session> <START或RESUME或CANCEL> <队列SHA256>
```

| 命令 | 用途 |
| --- | --- |
| START | 开始新批次；有活动/未完成批次时拒绝 |
| RESUME | 恢复匹配已保存队列的未结束批次；清旧归属和暂停错误，检查目标缺席 |
| CANCEL | 记录取消，按归属检查尝试结束本轮目标，保留已生成文件 |

`control.py` 读 ready/status；RESUME/CANCEL 还要读取本地 `jobs/<batch>/queue.txt`。只把 ready/status 下载到电脑不足以生成恢复命令。尚未消费的 command 或 client-pending 拒绝覆盖。FTP 不支持重命名覆盖时，先保留旧文件，再切换已验证候选；不能删除旧批次以完成替换。

## 持久化和结果

控制目录为 `ux0:data/trophy_bootstrap/`。

| 文件 | 用途 |
| --- | --- |
| ready.json | 当前启动 session、插件版本；重启生成新 session |
| status.json | 观察 phase、active、needs_explicit_resume、index/count、error、operation、waiting_for、storage_error |
| state-0.bin / state-1.bin | 交替 journal，序号及 SHA-256 校验；恢复以有效最新槽为准 |
| jobs/<batch>/queue.txt | 原批次不可变队列 |
| jobs/<batch>/result.json | 终态时输出逐项结果；暂停时可能尚不存在 |
| diagnostic.log | 状态转换、应用身份和错误位置；不记授权或 DAT 内容 |

每次转换先写临时文件、同步和回读，再替换对应槽。两槽都无有效数据时拒绝继续。journal 使用 V1 布局，status.error 优先报告负的控制/运行错误，否则显示持久化批次错误。

| 逐项结果 | 含义 |
| --- | --- |
| existing_skipped | 处理时已存在非空配对，跳过 |
| pair_generated_waiting_decrypted_export | 退出后观察到非空配对 |
| pair_recovered_waiting_decrypted_export | 暂停恢复时观察到已有配对 |
| missing_pair / incomplete_pair | 配对缺失 / 不完整，不能当成功 |
| launch_failed / cancelled / pending | 启动失败 / 取消 / 尚未处理 |

`BATCH_GENERATED` 仅表示全队列已处理；可包含 missing_pair。`PAUSED` 保留当前位置和现场，恢复可能在当前项，也可能已推进到下一项，必须看 index 和结果。error=0 不能替代逐项成功判定。

| 内部错误 | 十进制 | 含义 |
| --- | --- | --- |
| TB_ERROR | -29697 | 输入或通用契约错误 |
| TB_BUSY | -29698 | 目标或其他应用占用 |
| TB_CAPACITY | -29699 | 容量不足 |
| TB_IDENTITY | -29700 | 进程/队列/身份不一致 |
| TB_TIMEOUT | -29701 | 启动或退出观察超时 |
| TB_STORAGE | -29702 | 持久化失败 |

系统 API 错误可能为其他负数，结合 operation 定位，不按上述内部码替换解释。

## 宿主控制工具

`tools/control.py` 只处理电脑可访问的本地目录或挂载路径，不自行连接 FTP、上传或安装游戏。

1. 核对完整 catalog、实际安装 SFO/奖杯资源映射、已有个人配对与可用容量，准备新批次。
2. 将队列放入设备 `ux0:data/trophy_bootstrap/queue.txt`，基于当前 ready/status 会话发布 START。
3. 核对命令已消费、active=true；停止 FTP/USB，并在 LiveArea 彻底关闭 VitaShell，让插件处理队列。
4. 结束或暂停后重新连接，读取 status、journal、不可变队列与逐项结果，回读实际 DAT 配对。恢复前核对当前位置和目标运行状态。

以下命令中的路径是占位符，确认实际目标与副作用后执行：

```sh
python3 tools/control.py prepare --catalog /路径/catalog.json --output /路径/新批次 --limit 10 --user 00 --max-run-ms 30000
python3 tools/control.py command START --root /路径/设备data/trophy_bootstrap
python3 tools/control.py status --root /路径/设备data/trophy_bootstrap
python3 tools/control.py command RESUME --root /路径/设备data/trophy_bootstrap
python3 tools/control.py command CANCEL --root /路径/设备data/trophy_bootstrap
```

## 构建与验证

```sh
sh tools/test.sh
python3 tests/test_control.py
python3 tests/test_vita_format.py
sh tools/build.sh
python3 tools/verify_plugin.py /绝对路径/trophy_bootstrap.suprx
```

按实际变更选择检查。文档修改只查链接、内容和 diff，不运行设备任务或编译。队列、进程归属、容量、取消和故障恢复修改运行相关合成检查；SDK 构建与 SELF 检查不能替代设备验证。

`tools/build.sh` 使用固定摘要的 VitaSDK 镜像：源码只读、容器断网，输出到宿主临时目录。构建制品放入被忽略的 `artifacts/`。

固定 SDK 的 libk 可变参数存在 ARM 64 位对齐差异；日志中的 64 位数拆成两个 32 位十六进制值，不能重新引入混合 `%llu` 调用。格式回归由 `tests/test_vita_format.py` 检查。

本仓库初始化后没有构建或实机验收记录。完成报告明确区分静态、合成、构建和实机证据，简明列出实际变化、完成检查与未验证边界。

## 私人数据

实际账号、授权、DAT、私人队列、设备配置和研究日志不进入版本控制。工作现场及验证证据放入被忽略的 `local/`，按 NPWR 区分已生成、原有和缺失；新增设备结果必须有对应回读证据。不覆盖旧备份，不随任务清理设备数据。

## 许可证与源码来源

本项目使用 GPL-3.0-or-later，许可证全文见 [LICENSE](LICENSE)，源码中的 SPDX 标识保留。

`src/sha256.c` 与 `include/sha256.h` 改编自 GPL-3.0-or-later 源码基线 `63acbc8d8051ad1407fc88e8790edf4632f56618`，修改为常驻插件构建选择 libk 头文件。

固定 VitaSDK 构建链接 libk（PDCLib 公有领域 C 库）及 GCC 编译支持例程。本项目不分发完整工具链，工具链依赖的上游许可仍适用。

`include/app_status.h` 的 AppMgr 状态契约独立表示自 VitaSDK 面向 FW 3.60 的 API 布局：<https://github.com/vitasdk/vita-headers/blob/master/include/psp2/appmgr.h>。固定 SDK 提供状态导入桩，但缺少带类型的声明。没有复制固件二进制、游戏代码或签名材料。
