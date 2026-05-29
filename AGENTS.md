# AGENTS.md

# Codex 项目工作说明

## 1. 项目背景

本项目原本面向 **全志 T113-S3** 平台开发，现在需要迁移并适配到 **RK3568** 平台。

本次迁移的目标不是重写整个项目，而是在尽量保留原有 T113-S3 支持的基础上，新增 RK3568 平台支持。

Codex 在本项目中的主要任务包括：

1. 分析原 T113-S3 项目结构
2. 找出 Allwinner / T113-S3 / sunxi 相关依赖
3. 建立 RK3568 平台适配层
4. 修改构建系统
5. 修复编译错误
6. 通过 SSH/SCP 部署到 RK3568 开发板测试
7. 根据日志继续迭代修改

---

## 2. 回复和工作语言

Codex 默认使用中文回答。

所有修改说明、风险说明、验证方式、Git 提交建议，均尽量使用中文描述。

---

## 3. 总体工作原则

Codex 必须遵守以下原则：

1. 不要一次性大范围重构。
2. 每轮只完成一个明确的小目标。
3. 修改代码前必须先分析现状。
4. 不确定的硬件信息不要猜测。
5. 不要删除原有 T113-S3 代码，除非用户明确要求。
6. RK3568 适配应优先通过新增平台层、配置文件或构建 target 实现。
7. 每轮修改后必须说明改了什么、为什么改、如何验证。
8. 涉及危险命令时必须先请求用户确认。

---

## 4. Git 安全规则

### 4.1 修改前必须检查 Git 状态

在修改任何文件前，必须先执行：

```bash
git status --short
git branch --show-current
```

如果当前分支是：

```text
main
master
```

不要直接修改文件，应提醒用户先创建迁移分支。

推荐迁移分支名称：

```bash
port/t113-to-rk3568
```

创建分支命令：

```bash
git switch -c port/t113-to-rk3568
```

如果 `git switch` 不可用，使用：

```bash
git checkout -b port/t113-to-rk3568
```

---

### 4.2 如果工作区不干净

如果 `git status --short` 显示已有未提交修改，Codex 不要继续大范围修改。

应先说明：

1. 当前有哪些未提交文件
2. 这些文件是否可能与本轮任务冲突
3. 是否建议用户先提交、暂存或 stash

可建议用户使用：

```bash
git stash push -u -m "wip: before rk3568 porting"
```

但不要自动执行，除非用户明确要求。

---

### 4.3 修改后必须查看差异

每轮修改完成后，必须执行：

```bash
git status --short
git diff --stat
git diff
```

然后总结：

1. 修改了哪些文件
2. 每个文件为什么修改
3. 是否影响 T113-S3 原有路径
4. 是否影响 RK3568 新路径
5. 如何验证
6. 建议的 commit message

---

### 4.4 禁止自动执行的 Git 命令

除非用户明确要求，Codex 不要自动执行：

```bash
git add .
git commit
git push
git reset --hard
git clean -fd
git restore .
git checkout .
```

提交应由用户确认后执行。

如果需要建议提交，优先建议：

```bash
git add -p
git commit -m "build: add rk3568 target"
```
### 4.5 严格遵守 Ignore 规则
Codex 在执行任何 Git 状态检查或暂存操作时，必须：
1. 绝对无视 `build/` 目录下的任何 CMake/Make 临时产物。
2. 绝对无视工作区内自身产生的临时测试目录（如 `.codex-check-audio` 等）。
3. 发现误添加的临时文件时，优先使用 `git restore --staged <file>` 将其移出暂存区，禁止直接提交垃圾文件。
---

## 5. 推荐 Git 提交拆分

RK3568 迁移建议按小步提交，不要做成一个巨大 commit。

推荐提交粒度：

```text
chore: import original T113-S3 project
docs: add rk3568 porting notes
build: add rk3568 target
toolchain: add aarch64 cross compile config
platform: add rk3568 platform skeleton
platform: separate t113 and rk3568 implementations
drivers: adapt gpio backend for rk3568
drivers: adapt i2c and spi device paths for rk3568
scripts: add rk3568 ssh deploy script
fix: resolve rk3568 build errors
```

如果涉及设备树或内核驱动，可增加：

```text
dts: add rk3568 board device tree overlay
kernel: add rk3568 driver config
```

---

## 6. T113-S3 到 RK3568 迁移原则

### 6.1 不要直接覆盖 T113-S3 代码

原有 T113-S3 代码应尽量保留。

不推荐：

```text
直接把 t113 相关代码改成 rk3568
直接删除 allwinner/sunxi 相关实现
直接修改旧平台逻辑导致 T113-S3 无法继续编译
```

推荐：

```text
新增 RK3568 平台适配层
新增 RK3568 构建 target
新增 RK3568 配置文件
通过宏、配置、CMake option、Make target 区分平台
```

---

### 6.2 推荐目录结构

优先采用类似结构：

```text
platform/
  t113/
  rk3568/

config/
  t113.conf
  rk3568.conf

scripts/
  deploy_rk3568.sh
  run_rk3568.sh
```

如果原项目已有平台层，应尽量沿用原有设计，不要另起一套混乱结构。

---

### 6.3 必须扫描的平台相关关键词

修改前应搜索以下关键词：

```text
T113
t113
Allwinner
allwinner
sunxi
SUNXI
tina
lichee
arm-linux-gnueabi
arm-none-linux-gnueabihf
```

常用搜索命令：

```bash
grep -RIn "T113\|t113\|Allwinner\|allwinner\|sunxi\|SUNXI\|tina\|lichee" .
```
### 6.4 C++ 编码规范与 32/64 位架构适配
在重构和编写新代码时，Codex 必须严格遵守以下工程规范：

1. **架构字长警惕**：T113-S3 为 32 位，RK3568 为 64 位。迁移底层硬件控制或网络数据包解析代码时，必须仔细审查 `long`、`size_t` 以及指针强转（Pointer Casting）带来的隐患，注意 64 位下的结构体内存对齐（Struct Padding）。
2. **现代 C++ 标准**：严格遵循 C++11/17 标准，逐步将老旧的 C 风格 API 调用进行安全封装。
3. **内存与资源管理**：强制落实 RAII 原则。禁止在涉及对象所有权转移时滥用裸指针和手动 `new/delete`，必须使用 `std::shared_ptr`、`std::unique_ptr` 等智能指针，防范音视频流处理中的内存泄漏。
4. **多媒体解耦**：识别原有的全志私有 API（如 CedarX），将其替换为标准 FFmpeg API 或瑞芯微 RKMPP 接口。
---

## 7. 硬件相关迁移规则

如果项目涉及硬件访问，必须特别谨慎。

重点检查：

```text
GPIO
I2C
SPI
UART
PWM
ADC
CAN
LCD
Camera
Audio
Video
NPU
VPU
DRM
Framebuffer
```

重点检查路径：

```text
/dev/gpiochip*
/dev/i2c*
/dev/spidev*
/dev/ttyS*
/dev/ttyUSB*
/sys/class/gpio
/sys/class/pwm
/sys/bus/iio
/sys/class/drm
/dev/video*
/dev/fb*
```

不允许凭空猜测：

```text
GPIO 编号
I2C 总线号
SPI 总线号
UART 设备节点
PWM 通道
屏幕接口
摄像头接口
音频 codec
```

如果没有原理图、设备树或板级文档，应标记：

```text
TODO: 需要根据 RK3568 原理图或设备树确认
```

---

## 8. RK3568 开发板使用规则

RK3568 开发板只作为测试目标，不建议在板子上运行 Codex。

Codex 应运行在：

```text
Windows 本机
WSL2
Ubuntu 虚拟机
Ubuntu 编译服务器
```

RK3568 开发板通过 SSH/SCP 进行测试。

默认开发板 IP：

```text
192.168.137.30
```

默认 SSH 用户根据实际系统确定，常见为：

```text
root
```

连接示例：

```bash
ssh root@192.168.137.30
```

上传示例：

```bash
scp ./build/app root@192.168.137.30:/tmp/
```

运行示例：

```bash
ssh root@192.168.137.30 "chmod +x /tmp/app && /tmp/app"
```

如果用户名、路径或程序名不确定，必须询问用户，不要猜测。

---

## 9. 编译规则

### 9.1 优先在本机或编译主机交叉编译

不要默认在 RK3568 开发板上编译大型项目或 BSP。

优先使用：

```text
Windows + WSL2
Ubuntu 虚拟机
Ubuntu 编译服务器
交叉编译工具链
```

RK3568 通常为 ARM64/aarch64 平台，因此应优先检查：

```bash
aarch64-linux-gnu-gcc --version
```

如果项目使用 CMake，应检查：

```bash
cmake --version
```

如果项目使用 Make，应检查：

```bash
make --version
```

---

### 9.2 修改构建系统时的原则

修改 Makefile、CMakeLists.txt 或 build.sh 时：

1. 不要破坏原 T113-S3 target
2. 新增 RK3568 target
3. 明确区分 T113-S3 和 RK3568 工具链
4. 不要写死用户本机绝对路径
5. 不要写死临时测试路径
6. 不要把板子 IP 写进核心构建逻辑
7. 必须保证外部库（如 FFmpeg、Opus、Nginx-RTMP 依赖）的动态库（.so）或静态库（.a）路径在交叉编译环境下正确链接。
8. 优先使用 CMake 的 `find_package` 或 `pkg_check_modules`，而不是硬编码包含路径。
9. 所有的构建输出必须在独立的 `build/` 目录中进行（Out-of-source build），严禁污染源码目录。
推荐新增类似选项：

```text
TARGET=t113
TARGET=rk3568
```

或：

```text
-DTARGET_PLATFORM=rk3568
```

---

## 10. 部署和测试规则

如果生成了 RK3568 可执行程序，推荐流程：

```bash
scp ./build/app root@192.168.137.30:/tmp/app
ssh root@192.168.137.30 "chmod +x /tmp/app && /tmp/app"
```

运行失败后，收集：

```bash
echo $?
dmesg | tail -200
# 针对 RK3568 的多媒体与底层硬件，重点关注 MPP 和内核报错
dmesg | grep -Ei "rk_mpp|rockchip|drm|gpio|i2c|spi|uart|audio|codec|ffmpeg"
journalctl -n 200 --no-pager 2>/dev/null || true
ldd /tmp/app 2>/dev/null || true
```

如果是驱动、设备节点、权限问题，应继续检查：

```bash
ls -l /dev
ls /dev/i2c* /dev/spidev* /dev/ttyS* /dev/ttyUSB* 2>/dev/null || true
dmesg | grep -Ei "gpio|i2c|spi|uart|pwm|can|video|drm|audio|codec|camera"
```

---

## 11. 串口调试规则

如果 SSH 不可用，或系统启动异常，可使用串口兜底调试。

串口调试时注意：

1. MobaXterm 和 Python 脚本通常不能同时占用同一个 COM 口。
2. 如果串口输出乱码，优先检查波特率。
3. RK3568 常见波特率可能是 `1500000` 或 `115200`。
4. 串口日志应保存到 `logs/` 目录。
5. 串口只用于调试，不要自动执行危险命令。

建议日志文件：

```text
logs/serial_boot.log
logs/serial_dmesg.log
logs/rk3568_run.log
```

---

## 12. 禁止自动执行的危险命令

除非用户明确要求并确认，Codex 不要自动执行：

```bash
rm -rf
dd
mkfs
reboot
poweroff
shutdown
flash_erase
nandwrite
parted
fdisk
mount
umount
```

尤其不要对开发板存储设备执行写盘、格式化、擦除、分区相关命令。

---

## 13. 每轮任务的标准流程

每轮任务必须按以下流程进行：

### 第一步：确认状态

```bash
git status --short
git branch --show-current
```

### 第二步：说明本轮计划

必须先告诉用户：

```text
本轮准备修改什么
不会修改什么
预计影响哪些文件
如何验证
```

### 第三步：执行小范围修改

每轮只做一个小目标，例如：

```text
只扫描项目结构
只新增 rk3568 target
只新增 platform/rk3568
只修改工具链配置
只修复一个编译错误
只新增部署脚本
```

### 第四步：检查 diff

```bash
git status --short
git diff --stat
git diff
```

### 第五步：总结

必须输出：

```text
1. 修改文件列表
2. 修改原因
3. 是否影响 T113-S3
4. 是否影响 RK3568
5. 编译/运行验证方式
6. 风险点
7. 建议 commit message
```

---

## 14. 第一轮任务建议

第一次接手本项目时，不要直接修改代码。

第一轮只做扫描分析，输出：

```text
1. 项目整体结构
2. 构建系统类型
3. T113-S3 / Allwinner / sunxi 相关依赖
4. 硬件相关模块清单
5. 需要迁移到 RK3568 的模块
6. 当前不确定的信息
7. 第一轮最小修改建议
8. 推荐 Git 提交拆分方案
```

---

## 15. 建议的第一条 Codex 指令

用户可以这样开始：

```text
请先读取 AGENTS.md。

本轮不要修改代码。

请先执行 git status --short 和 git branch --show-current。

然后扫描项目结构，输出：
1. 项目构建方式
2. T113-S3 / Allwinner / sunxi 相关依赖
3. 硬件相关模块清单
4. 迁移到 RK3568 的风险点
5. 第一轮最小修改建议
6. 推荐 Git 提交拆分方案
```