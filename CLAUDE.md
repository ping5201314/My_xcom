# my_com 串口助手 V1.0.0.1

Qt 6.8.3 + MinGW 的 Windows 串口助手（Finca 咖啡机主控调试上位机）。GUI 有串口助手/固件升级两个页签；另有**命令行模式**供 AI/脚本直接控制串口。

## 版本管理

版本号统一定义在 `src/version.h`（`MYCOM_VERSION_STR` 等宏）。改版只改这一个文件；`my_com.rc` 的 VERSIONINFO 数字需手动同步（Explorer 属性可见）。

## 构建

```bash
cmake -S . -B build/vscode/Debug -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH=D:/15306/QT68/QT/6.8.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vscode/Debug -j 8
# Release 发行版: build/release（dist/ 是 windeployqt 打包的免安装版）
```

测试开关：`-DMYCOM_BUILD_TESTS=ON`（默认关）。

## AI 控制串口（命令行模式）

exe：`dist/my_com.exe`（发行版）或 `build/verify/Debug/my_com.exe`。

```bash
# 自动端口（推荐）：不写 --port 时中转优先
my_com.exe cli send "@R20:0" --wait-ms 8000

# 强制指定端口直连（跳过中转）
my_com.exe cli send "@R20:0" --port COM5 --wait-ms 8000

# 监听 N 秒
my_com.exe cli listen --port COM5 --seconds 10

# 无界面固件升级（bin ≤120K；升级全程约 30 秒）
my_com.exe cli upgrade <固件.bin> --port COM5
```

- **自动端口逻辑**：① 中转优先——GUI 运行且串口已开 → 经它代发（与端口号无关）；② 中转不可用 → 回退直连 GUI 配置（%APPDATA% ini）里的 lastPort；③ 都没有 → 报错提示指定 --port
- 输出：stdout + **exe 旁 cli.log**（无控制台环境读文件最可靠）；RX 带「RX文本:」可读文本
- 所有 CLI 与 GUI 的收发统一写入 `%TEMP%/my_com_trace.log`（共享监视文件）
- 退出码 0=成功、1=失败
- 常用调试配方：@R20:0 查版本、@R06:0 查工作状态、@R30:0 查报警位图

## 人机协同（多数场景：先人工 GUI，后 AI）

1. **GUI 占用 + AI 代发（默认路径）**：用户 GUI 已连串口 → AI `cli send` 自动中转（请求文件 `%TEMP%/my_com_ai_tx.txt`，GUI 每 500ms 轮询代发）——**人与 AI 同一串口同时操作，互不影响**
2. **AI 独占 + 人监视**：GUI 未占用串口时 AI 直连；人勾选 GUI「监视AI会话」实时看 AI 收发
3. **端口感知**：GUI 开/关串口写 `GUI-OPEN: COMx @波特率` / `GUI-CLOSED` 进共享记录；勾选状态存 `%APPDATA%/MyTeam/my_com.ini`（timestamp/autoScroll/encoding 等键），AI 可直接读

**监视显示规则**：设备回复紫色无前缀；人发送青色；AI 动作带 `[AI]`；CLI-ERROR 不显示（AI 读 cli.log）；时间戳跟随「时间戳」勾选框；回显按完整行聚合（relay 尾部回显不回写共享文件，防重复显示）。

## 关键规则

1. **固件工程只读**：`d:\MY_WORK\Finca` 的代码只能查看分析，不修改（用户明确要求；改动固件需用户逐次明确授权）
2. **不自动提交 git**：提交/推送需用户明确要求
3. 固件源码是 GBK 编码，读取用 `iconv -f GBK -t UTF-8`
4. 设备回显行尾 `\r\r\n`、打印慢（约 18 字节/秒）；升级成功判据 = ①优先检测设备重启（BOOT 打印 "boot version"/"write percent"，10s 窗口）②否则轮询 @R20 回显含 App_version 即成功（不比对版本号数值，2026-09-17 用户拍板）
5. 指令表：运行时读写 `%APPDATA%\MyTeam\my_com\debug_commands.csv`（Roaming；Qt 6 AppConfigLocation 指向 Local，勿用）——6 列「分组,名称,发送内容,带参数,默认参数,参数提示」UTF-8 BOM，Excel 双击可编辑（读入兼容 GBK 无 BOM，2026-10-21 用户拍板）；旧 json（固定位置或 exe 旁）在 csv 缺失时自动迁移为 csv；「加载表格」支持 csv/json；POST_BUILD 只做"种子复制"（文件不存在才复制）
