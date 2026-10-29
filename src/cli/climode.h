#pragma once

// 命令行模式（供 AI/脚本直接控制串口，无界面）：
//   my_com.exe cli send  <帧文本>  --port COM5 [--baud 115200] [--wait-ms 3000]
//   my_com.exe cli listen          --port COM5 [--baud 115200] [--seconds 5]
//   my_com.exe cli upgrade <bin>   --port COM5
// 输出全部为 stdout 文本（HEX+可读），退出码 0=成功 1=失败——便于 AI（Claude Code）
// 在终端里调用并读取结果来调试设备。
int runCliMode(int argc, char* argv[]);
