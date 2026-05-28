#pragma once

#include "theseed/login/LoginTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace theseed::login {

enum class ClientMessageType : std::uint8_t {
    // LoginApp messages
    Login = 1,
    QueryRealms = 2,
    SelectRealm = 3,

    LoginResponse = 101,
    QueryRealmsResponse = 102,
    SelectRealmResponse = 103,
    ErrorResponse = 200,

    // BaseApp client messages
    EnterGame = 10,
    EnterGameResponse = 110,
    EntityEnter = 111,
    EntityLeave = 112,
    PropertySync = 113,
};

class LoginProtocol {
public:
    // 消息帧: [4字节 payload 长度][1字节 类型][payload]
    static constexpr std::size_t kHeaderSize = 5;

    static std::vector<std::byte> frameMessage(ClientMessageType type,
                                                std::span<const std::byte> payload);

    static bool parseFrame(std::span<const std::byte> data,
                           ClientMessageType& outType,
                           std::span<const std::byte>& outPayload);

    // 编码服务端 → 客户端
    static std::vector<std::byte> encodeLoginResponse(const LoginResponse& resp);
    static std::vector<std::byte> encodeRealmList(const std::vector<RealmInfo>& realms);
    static std::vector<std::byte> encodeSelectRealmResponse(const SelectRealmResponse& resp);
    static std::vector<std::byte> encodeError(const std::string& message);

    // 解码客户端 → 服务端
    static bool decodeLogin(std::span<const std::byte> payload,
                            std::string& outAccount,
                            std::string& outPassword);
    static bool decodeSelectRealm(std::span<const std::byte> payload,
                                  std::string& outRealmId);
};

}  // namespace theseed::login
