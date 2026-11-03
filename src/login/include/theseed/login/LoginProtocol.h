#pragma once

#include "theseed/login/LoginTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <string>

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
    Action = 11,
    EnterGameResponse = 110,
    EntityEnter = 111,
    EntityLeave = 112,
    PropertySync = 113,
    ActionForward = 114,
    EntityEvent = 115,
    SpaceChange = 116,
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

    // 编码客户端 → 服务端（bot / 真实客户端的请求构造面，与 decode 镜像）
    static std::vector<std::byte> encodeLogin(const std::string& account,
                                              const std::string& password);
    static std::vector<std::byte> encodeSelectRealm(const std::string& realmId);

    // 解码客户端 → 服务端
    static bool decodeLogin(std::span<const std::byte> payload,
                            std::string& outAccount,
                            std::string& outPassword);
    static bool decodeSelectRealm(std::span<const std::byte> payload,
                                  std::string& outRealmId);

    // 解码服务端 → 客户端（bot / 真实客户端的应答解析面）
    static bool decodeLoginResponse(std::span<const std::byte> payload,
                                    LoginResponse& outResp);
    static bool decodeRealmList(std::span<const std::byte> payload,
                                std::vector<RealmInfo>& outRealms);
    static bool decodeSelectRealmResponse(std::span<const std::byte> payload,
                                          SelectRealmResponse& outResp);
    static bool decodeError(std::span<const std::byte> payload,
                            std::string& outMessage);
};

}  // namespace theseed::login
