#pragma once

#include "theseed/foundation/RedisProvider.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace theseed::foundation {

// MVP Phase B-3: token-based session store on top of IRedisProvider.
// Sessions are opaque blobs keyed by token; the store adds TTL semantics.
// In a multi-process deployment, an injected hiredis-backed provider makes
// the same tokens visible to every LoginApp / BaseApp.
//
// Enumeration (listSessions) rides a secondary zset index (member = token,
// score = save-time epoch millis) because the provider primitives have no
// keyspace scan. The index lives in the same provider, so every process
// sharing the provider shares the same view. Index and session key are two
// writes and cannot be atomic — stale index members (expired sessions,
// corrupt blobs) are retired lazily at enumeration time.

struct StoredSession final {
    std::string accountId;
    std::string realmId;
    std::int64_t userId = 0;
    std::string metadata;  // opaque blob (e.g. JSON) for client-specific state
};

// Enumeration row. `token` is the raw login credential: callers may use it
// to keep operating on the session (e.g. batch revoke) but must never log
// or echo it — protocol surfaces redact to a length fingerprint.
struct SessionView final {
    std::string token;
    std::string accountId;
    std::string realmId;
    std::int64_t userId = 0;
};

class SessionStore final {
public:
    explicit SessionStore(std::shared_ptr<IRedisProvider> redis);

    // Saves the session under `token` with the given TTL and registers it
    // in the enumeration index. Returns false if redis_ is null, the token
    // is empty, or the index write fails. Encoding is line-delimited and
    // intentionally simple — it is not a wire format meant to cross engine
    // versions.
    bool save(const std::string& token,
              const StoredSession& session,
              RedisDuration ttl);

    std::optional<StoredSession> load(const std::string& token);

    // Refresh the TTL on an existing token. Returns false if missing.
    bool refresh(const std::string& token, RedisDuration ttl);

    // Remove the token. Returns false if not found. A hit also retires the
    // index member; a miss leaves it (expired residue is retired lazily by
    // listSessions).
    bool revoke(const std::string& token);

    // Live sessions currently visible through the shared index. Order is
    // provider-defined (the in-memory provider yields member-lexicographic
    // order). Retires stale index members (expired session key, corrupt
    // blob) as a side effect.
    std::vector<SessionView> listSessions();

    static std::string encode(const StoredSession& session);
    static std::optional<StoredSession> decode(const std::string& blob);

private:
    std::shared_ptr<IRedisProvider> redis_;
};

}  // namespace theseed::foundation
