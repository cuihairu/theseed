// BotClient 单元测试：不依赖真实网络，用自定义管道驱动 InGame 发送失败路径，
// 验证同一断线事件内 move 与 heartbeat 均发送失败时 reconnects 只计一次。
#include "BotClient.h"
#include "BotsConfig.h"

#include "theseed/login/ClientSession.h"
#include "theseed/runtime/IBytePipe.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <span>

using theseed::bots::BotsConfig;
using theseed::bots::BotClient;
using theseed::runtime::IBytePipe;

namespace {

// 管道：isConnected 初始为 true；首次 write 返回 false 并断开连接（模拟
// ClientSession::send 写失败即 close 的副作用），此后 isConnected 为 false。
class FailOncePipe final : public IBytePipe {
public:
    bool write(std::span<const std::byte>) override {
        connected_ = false;
        return false;
    }
    void pump() override {}
    void setOnReceived(std::function<void(std::span<const std::byte>)>) override {}
    void close() override { connected_ = false; }
    bool isConnected() const override { return connected_; }

private:
    bool connected_ = true;
};

}  // namespace

namespace theseed::bots {

// 测试访问器：把 bot 注入 InGame 且 move/heartbeat 均已到期，使同一 tick 内
// 两次 sendAction 都会进入发送失败路径（首次失败已 close，第二次走已断分支）。
class BotClientTest {
public:
    static void injectInGame(BotClient& bot,
                             std::shared_ptr<FailOncePipe> pipe,
                             std::chrono::steady_clock::time_point now) {
        bot.baseSession_ = std::make_unique<login::ClientSession>(pipe);
        bot.state_ = BotClient::State::InGame;
        bot.entityId_ = 1;
        bot.lastMoveAt_ = now - std::chrono::hours(1);
        bot.lastHeartbeatAt_ = now - std::chrono::hours(1);
    }
};

}  // namespace theseed::bots

int main() {
    std::cout << "BotClient tests:\n";
    int failed = 0;

    {
        BotsConfig cfg;
        cfg.moveInterval = std::chrono::milliseconds{1};
        cfg.heartbeatInterval = std::chrono::milliseconds{1};
        BotClient bot(cfg, "bot_0");
        auto pipe = std::make_shared<FailOncePipe>();
        const auto now = std::chrono::steady_clock::now();
        theseed::bots::BotClientTest::injectInGame(bot, pipe, now);

        bot.tick(now);

        std::cout << "  double send failure counts reconnect once... ";
        if (bot.stats().reconnects != 1) {
            std::cout << "FAILED: reconnects=" << bot.stats().reconnects << "\n";
            ++failed;
        } else {
            std::cout << "OK\n";
        }

        const auto retryAt = now + cfg.reconnectDelay;
        bot.tick(retryAt);
        bot.forceDisconnect(retryAt);

        std::cout << "  failure after retry starts a new event... ";
        if (bot.state() != BotClient::State::Backoff ||
            bot.stats().reconnects != 2) {
            std::cout << "FAILED: state=" << static_cast<int>(bot.state())
                      << " reconnects=" << bot.stats().reconnects << "\n";
            ++failed;
        } else {
            std::cout << "OK\n";
        }
    }

    std::cout << "BotClient: " << (failed == 0 ? "all passed" : "FAILED")
              << "\n";
    return failed == 0 ? 0 : 1;
}
