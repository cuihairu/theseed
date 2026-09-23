#pragma once

#include "theseed/foundation/RedisProvider.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace theseed::foundation {

// MVP Phase B-3: token-based session store on top of IRedisProvider.
// Sessions are opaque blobs keyed by token; the store adds TTL semantics.
// In a multi-process deployment, an injected hiredis-backed provider makes
// the same tokens visible to every LoginApp / BaseApp.

struct StoredSession final {
    std::string accountId;
    std::string realmId;
    std::int64_t userId = 0;
    std::string metadata;  // opaque blob (e.g. JSON) for client-specific state
};

class SessionStore final {
public:
    explicit SessionStore(std::shared_ptr<IRedisProvider> redis);

    // Saves the session under `token` with the given TTL. Returns false if
    // redis_ is null. Encoding is line-delimited and intentionally simple —
    // it is not a wire format meant to cross engine versions.
    bool save(const std::string& token,
              const StoredSession& session,
              RedisDuration ttl);

    std::optional<StoredSession> load(const std::string& token);

    // Refresh the TTL on an existing token. Returns false if missing.
    bool refresh(const std::string& token, RedisDuration ttl);

    // Remove the token. Returns false if not found.
    bool revoke(const std::string& token);

    static std::string encode(const StoredSession& session);
    static std::optional<StoredSession> decode(const std::string& blob);

private:
    std::shared_ptr<IRedisProvider> redis_;
};

}  // namespace theseed::foundation
