#include "theseed/login/SessionToken.h"

#include <random>
#include <sstream>

namespace theseed::login {

static constexpr char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned char decodeLookup(char c) {
    for (int i = 0; i < 64; ++i) {
        if (kEncodeTable[i] == c) return static_cast<unsigned char>(i);
    }
    return 0;
}

static std::string base64Encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        auto b0 = static_cast<unsigned char>(input[i]);
        auto b1 = static_cast<unsigned char>(input[i + 1]);
        auto b2 = static_cast<unsigned char>(input[i + 2]);
        out += kEncodeTable[b0 >> 2];
        out += kEncodeTable[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += kEncodeTable[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out += kEncodeTable[b2 & 0x3F];
    }
    if (i < input.size()) {
        auto b0 = static_cast<unsigned char>(input[i]);
        out += kEncodeTable[b0 >> 2];
        if (i + 1 < input.size()) {
            auto b1 = static_cast<unsigned char>(input[i + 1]);
            out += kEncodeTable[((b0 & 0x03) << 4) | (b1 >> 4)];
            out += kEncodeTable[(b1 & 0x0F) << 2];
        } else {
            out += kEncodeTable[(b0 & 0x03) << 4];
            out += '=';
        }
        out += '=';
    }
    return out;
}

static std::string base64Decode(const std::string& input) {
    std::string out;
    out.reserve((input.size() * 3) / 4);

    unsigned char buf[4]{};
    int n = 0;
    for (char c : input) {
        if (c == '=') break;
        buf[n++] = decodeLookup(c);
        if (n == 4) {
            out += static_cast<char>((buf[0] << 2) | (buf[1] >> 4));
            out += static_cast<char>((buf[1] << 4) | (buf[2] >> 2));
            out += static_cast<char>((buf[2] << 6) | buf[3]);
            n = 0;
        }
    }
    if (n >= 2) out += static_cast<char>((buf[0] << 2) | (buf[1] >> 4));
    if (n >= 3) out += static_cast<char>((buf[1] << 4) | (buf[2] >> 2));
    return out;
}

static constexpr std::string_view kPrefix = "login:";

std::string SessionToken::issue(const std::string& accountId, const std::string& realmId) {
    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dist(0, 0xFFFFFFFF);
    auto nonce = dist(rd);

    std::ostringstream ss;
    ss << std::hex << nonce;
    auto raw = std::string(kPrefix) + accountId + ":" + realmId + ":" + ss.str();
    return base64Encode(raw);
}

bool SessionToken::validate(const std::string& token,
                            std::string& outAccountId,
                            std::string& outRealmId) {
    auto decoded = base64Decode(token);

    if (decoded.size() < kPrefix.size()) return false;
    if (decoded.substr(0, kPrefix.size()) != kPrefix) return false;

    auto rest = decoded.substr(kPrefix.size());

    auto col1 = rest.find(':');
    if (col1 == std::string::npos) return false;
    auto col2 = rest.find(':', col1 + 1);
    if (col2 == std::string::npos) return false;

    outAccountId = rest.substr(0, col1);
    outRealmId = rest.substr(col1 + 1, col2 - col1 - 1);
    return !outAccountId.empty() && !outRealmId.empty();
}

}  // namespace theseed::login
