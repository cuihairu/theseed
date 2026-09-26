#pragma once

#include <chrono>
#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::control::machine {

// 解析 /proc/net/tcp[tcp6] 文本流，收集 LISTEN 套接字（inode 字符串 → 端口）。
// 单独暴露以便测试用合成流驱动畸形行分支、工具链复用同一解析器。
// 表头行与列数不足/非 LISTEN/无冒号地址/零端口的行一律跳过；同一 inode
// 后出现的端口覆盖先值。
void collectListenInodes(std::istream& input,
                         std::unordered_map<std::string, std::uint16_t>& out);

// 端口占用扫描（Linux）：/proc/net/tcp[tcp6] 里的 LISTEN 套接字按 inode 收集，
// 再经 /proc/<pid>/fd 反查拥有者，得到“进程 → 监听端口”表。
//
// 边界：
// - 只反查调用方给出的 pid 集合，不做全机 fd 扫描；
// - 无权限读的 fd 目录（他人进程）整目录跳过，不报错——快照是观察而非审计；
// - 非 Linux 平台返回空表（todo 遗留：跨平台等价实现）。
std::unordered_map<std::uint32_t, std::uint16_t> scanListeningPorts(
    const std::vector<std::uint32_t>& pids);

// 二进制版本探测：对 127.0.0.1:port 发 GET /health，从响应 JSON 里取
// "version" 字段（theseed 各进程的 OpsServer 都暴露该端点）。
//
// 主动连接只发生在调用方显式给出的端口上——权限边界：listProcesses 只对
// 受管进程探测，不触碰非受管进程。失败/超时/响应无版本一律返回空串；
// 超时只覆盖收发（目标是本机回环，connect 不会长阻塞）。非 Windows 平台
// 之外暂返回空串（todo 遗留：winsock 版实现）。
std::string probeProcessVersion(
    std::uint16_t port,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

}  // namespace theseed::control::machine
