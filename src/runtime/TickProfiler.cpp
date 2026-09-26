#include "theseed/runtime/TickProfiler.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace theseed::runtime {

TickProfiler::TickProfiler() : TickProfiler(Config{}) {}

TickProfiler::TickProfiler(Config config) : config_(config) {
    // 容量纪律下界：环形至少留 1 格，0 视为 1（与"0 = 关闭"的审计环形
    // 语义不同——这里 0 只是配置笔误，不做静默关闭语义）。
    if (config_.maxArtifacts == 0) {
        config_.maxArtifacts = 1;
    }
}

std::uint64_t TickProfiler::trigger() {
    if (sampling_) {
        return 0;  // 限流：同一时刻只允许一个窗口
    }
    if (config_.windowTicks == 0) {
        return 0;  // 配置无效：窗口收不满，触发恒拒
    }

    samples_.clear();
    windowStart_ = std::chrono::steady_clock::now();
    sampling_ = true;
    openHandle_ = ++nextHandle_;  // 句柄在触发时即占号：查询端可立刻引用
    return openHandle_;
}

bool TickProfiler::sampling() const {
    return sampling_;
}

void TickProfiler::onTickCompleted(std::uint64_t /*tickIndex*/, Duration duration) {
    if (!sampling_) {
        return;  // 空闲 tick：静默忽略
    }

    samples_.push_back(
        std::chrono::duration<double, std::milli>(duration).count());
    if (samples_.size() == config_.windowTicks) {
        finalizeWindow();
    }
}

void TickProfiler::finalizeWindow() {
    // 墙钟跨度单行化：声明与取值分两条语句，规避 gcc 覆盖计数在多行
    // 调用表达式上的错归因（口径与其余统计变量一致）。
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - windowStart_;
    const double windowMs = elapsed.count();

    // 统计量单行化：命名变量既可读，也避开多行调用表达式的 gcc 覆盖
    // 计数错归因问题。
    const auto slowThresholdMs =
        static_cast<double>(config_.slowThreshold.count());
    double minMs = samples_.front();
    double maxMs = samples_.front();
    double sumMs = 0.0;
    std::uint64_t slowSamples = 0;
    for (const double sample : samples_) {
        minMs = std::min(minMs, sample);
        maxMs = std::max(maxMs, sample);
        sumMs += sample;
        if (sample > slowThresholdMs) {
            ++slowSamples;
        }
    }
    const double avgMs = sumMs / static_cast<double>(samples_.size());

    // 产物 JSON：字段顺序固定，数值无需转义；samples 保留采样序。
    std::ostringstream out;
    out << "{\"handle\":" << openHandle_
        << ",\"window_ticks\":" << samples_.size()
        << ",\"window_ms\":" << windowMs
        << ",\"slow_threshold_ms\":" << slowThresholdMs
        << ",\"slow_samples\":" << slowSamples
        << ",\"min_ms\":" << minMs << ",\"max_ms\":" << maxMs
        << ",\"avg_ms\":" << avgMs << ",\"samples\":[";
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << samples_[i];
    }
    out << "]}";

    Sampled artifact;
    artifact.handle = openHandle_;
    artifact.tickCount = samples_.size();
    artifact.windowMs = windowMs;
    artifact.payload = out.str();

    if (artifacts_.size() == config_.maxArtifacts) {
        artifacts_.erase(artifacts_.begin());  // 环形：满后丢最旧
    }
    artifacts_.push_back(std::move(artifact));

    sampling_ = false;
    openHandle_ = 0;
}

std::vector<ITickProfiler::ArtifactMeta> TickProfiler::listArtifacts() const {
    std::vector<ArtifactMeta> metas;
    metas.reserve(artifacts_.size());
    for (const auto& artifact : artifacts_) {
        ArtifactMeta meta;
        meta.handle = artifact.handle;
        meta.tickCount = artifact.tickCount;
        meta.windowMs = artifact.windowMs;
        metas.push_back(meta);
    }
    return metas;
}

bool TickProfiler::artifactPayload(std::uint64_t handle, std::string& out) const {
    for (const auto& artifact : artifacts_) {
        if (artifact.handle == handle) {
            out = artifact.payload;
            return true;
        }
    }
    return false;  // 未知句柄：从未产出，或已被环形逐出
}

}  // namespace theseed::runtime
