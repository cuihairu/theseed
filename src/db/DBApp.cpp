#include "theseed/db/DBApp.h"
#include "theseed/core/FileEntityStore.h"
#include "theseed/db/DBProtocol.h"
#if THESEED_HAS_MYSQL
#include "theseed/db/MySQLEntityStore.h"
#endif
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>

namespace theseed::db {

namespace {

class ScopedTimer final {
public:
    using Emitter = std::function<void(double)>;
    explicit ScopedTimer(Emitter emitter)
        : start_(std::chrono::steady_clock::now()), emitter_(std::move(emitter)) {}
    ~ScopedTimer() {
        if (emitter_) {
            emitter_(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start_).count());
        }
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::chrono::steady_clock::time_point start_;
    Emitter emitter_;
};

theseed::foundation::Histogram& dbHistogram(const char* name, const char* desc) {
    return theseed::foundation::MetricsRegistry::instance().histogram(
        name,
        theseed::foundation::Histogram::Boundaries{1.0, 5.0, 10.0, 25.0, 50.0,
                                                    100.0, 250.0, 500.0, 1000.0, 2500.0},
        desc);
}

}  // namespace

DBApp::DBApp(Config config)
    : config_(std::move(config)) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

DBApp::~DBApp() {
    stop();
}

bool DBApp::init() {
#if THESEED_HAS_MYSQL
    if (config_.storeBackend == "mysql") {
        MySQLEntityStore::Config mysqlCfg;
        mysqlCfg.mysql.host = config_.mysqlHost;
        mysqlCfg.mysql.port = config_.mysqlPort;
        mysqlCfg.mysql.user = config_.mysqlUser;
        mysqlCfg.mysql.password = config_.mysqlPassword;
        mysqlCfg.mysql.database = config_.mysqlDatabase;
        mysqlCfg.autoCreateSchema = config_.mysqlAutoCreateSchema;

        auto mysqlStore = std::make_shared<MySQLEntityStore>(std::move(mysqlCfg));
        if (!mysqlStore->init()) {
            std::cerr << "DBApp: MySQL store init failed: " << mysqlStore->lastError()
                      << std::endl;
            return false;
        }
        store_ = mysqlStore;
        accountStore_ = mysqlStore;  // MySQLEntityStore 同时实现 IAccountStore
    } else {
        store_ = std::make_shared<core::FileEntityStore>(config_.storePath);
        accountStore_ = nullptr;
    }
#else
    if (config_.storeBackend == "mysql") {
        std::cerr << "DBApp: storeBackend=mysql requested but theseed_db was built "
                  << "without MySQL support (libmysql not available). Falling back to file."
                  << std::endl;
        config_.storeBackend = "file";
    }
    store_ = std::make_shared<core::FileEntityStore>(config_.storePath);
    accountStore_ = nullptr;
#endif

    hub_ = std::make_shared<runtime::TransportHub>(config_.componentId);

    if (!listener_.listen(config_.listenHost, config_.listenPort)) {
        std::cerr << "DBApp: failed to listen on " << config_.listenHost
                  << ":" << config_.listenPort << std::endl;
        return false;
    }

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};
        info.role = "DBApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();
        info.componentId = config_.componentId;

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            if (store_) {
                rt.entityTypes = store_->listEntityTypes();
            }
            if (hub_) {
                rt.transportStats = hub_->stats();
            }
            return rt;
        });

        ops::OpsServer::Config opsCfg{};
        opsCfg.host = config_.ops.host;
        opsCfg.port = config_.ops.port;
        opsCfg.maxConnections = config_.ops.maxConnections;
        opsServer_ = std::make_unique<ops::OpsServer>(opsCfg, *opsInspector_);
        opsServer_->start();
    }

    return true;
}

void DBApp::tick() {
    acceptConnections();
    hub_->tick();
    processMessages();

    if (hub_) {
        transportStatsCollector_.collect(hub_->stats());
    }

    if (opsServer_) {
        opsServer_->tick();
    }
}

void DBApp::stop() {
    listener_.close();
    hub_.reset();
}

void DBApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        auto transport = std::make_shared<runtime::NetworkTransport>(conn);
        // Assign sequential component IDs to connected BaseApps
        static runtime::ComponentId nextPeer{1};
        auto peerId = nextPeer++;
        hub_->connectPeer(peerId, transport);
    }
}

void DBApp::processMessages() {
    runtime::RuntimeInvocation inv;
    while (hub_->receive(config_.componentId, &inv, 1) > 0) {
        handleInvocation(inv);
    }
}

void DBApp::handleInvocation(runtime::RuntimeInvocation& inv) {
    const auto& method = inv.method;

    if (method == DBMethod::kLoad) {
        handleLoad(inv);
    } else if (method == DBMethod::kSave) {
        handleSave(inv);
    } else if (method == DBMethod::kRemove) {
        handleRemove(inv);
    } else if (method == DBMethod::kAllocId) {
        handleAllocId(inv);
    } else if (method == DBMethod::kListIds) {
        handleListIds(inv);
    } else if (method == DBMethod::kListTypes) {
        handleListTypes(inv);
    } else if (method == DBMethod::kQueryAccount) {
        handleQueryAccount(inv);
    } else if (method == DBMethod::kCreateAccount) {
        handleCreateAccount(inv);
    }
}

void DBApp::handleLoad(const runtime::RuntimeInvocation& inv) {
    ScopedTimer timer([](double ms) {
        dbHistogram("db_load_ms", "entity load latency in milliseconds").observe(ms);
    });

    core::EntityId id;
    std::string entityType;
    if (!DBProtocol::decodeLoadRequest(inv.payload, id, entityType)) {
        auto resp = DBProtocol::encodeLoadResponse(false, {});
        sendResponse(inv.sourceComponent, DBMethod::kLoadOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    core::EntityData data;
    bool ok = store_->load(id, entityType, data);
    auto resp = DBProtocol::encodeLoadResponse(ok, data);
    sendResponse(inv.sourceComponent, DBMethod::kLoadOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleSave(const runtime::RuntimeInvocation& inv) {
    ScopedTimer timer([](double ms) {
        dbHistogram("db_save_ms", "entity save latency in milliseconds").observe(ms);
    });

    core::EntityId id;
    core::EntityData data;
    if (!DBProtocol::decodeSaveRequest(inv.payload, id, data)) {
        auto resp = DBProtocol::encodeSaveResponse(false);
        sendResponse(inv.sourceComponent, DBMethod::kSaveOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    bool ok = store_->save(id, data);
    auto resp = DBProtocol::encodeSaveResponse(ok);
    sendResponse(inv.sourceComponent, DBMethod::kSaveOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleRemove(const runtime::RuntimeInvocation& inv) {
    core::EntityId id;
    if (!DBProtocol::decodeRemoveRequest(inv.payload, id)) {
        auto resp = DBProtocol::encodeRemoveResponse(false);
        sendResponse(inv.sourceComponent, DBMethod::kRemoveOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    bool ok = store_->remove(id);
    auto resp = DBProtocol::encodeRemoveResponse(ok);
    sendResponse(inv.sourceComponent, DBMethod::kRemoveOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleAllocId(const runtime::RuntimeInvocation& inv) {
    auto id = store_->allocId();
    auto resp = DBProtocol::encodeAllocIdResponse(id);
    sendResponse(inv.sourceComponent, DBMethod::kAllocIdOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleListIds(const runtime::RuntimeInvocation& inv) {
    std::string entityType;
    auto payload = inv.payload;
    // Decode entityType from payload
    if (payload.size() < 4) return;
    std::size_t offset = 0;
    auto len = static_cast<std::uint32_t>(payload[0])
             | (static_cast<std::uint32_t>(payload[1]) << 8)
             | (static_cast<std::uint32_t>(payload[2]) << 16)
             | (static_cast<std::uint32_t>(payload[3]) << 24);
    offset = 4;
    if (offset + len <= payload.size()) {
        entityType.resize(len);
        if (len > 0) std::memcpy(entityType.data(), payload.data() + offset, len);
    }

    auto ids = store_->listIdsByType(entityType);
    auto resp = DBProtocol::encodeListIdsResponse(ids);
    sendResponse(inv.sourceComponent, DBMethod::kListIdsOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleListTypes(const runtime::RuntimeInvocation& inv) {
    auto types = store_->listEntityTypes();
    auto resp = DBProtocol::encodeListTypesResponse(types);
    sendResponse(inv.sourceComponent, DBMethod::kListTypesOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleQueryAccount(const runtime::RuntimeInvocation& inv) {
    ScopedTimer timer([](double ms) {
        dbHistogram("db_query_account_ms", "account query latency in milliseconds").observe(ms);
    });

    std::string username;
    if (!DBProtocol::decodeQueryAccountRequest(inv.payload, username)) {
        auto resp = DBProtocol::encodeQueryAccountResponse(false, 0, "");
        sendResponse(inv.sourceComponent, DBMethod::kQueryAccountOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    // 优先走 MySQL/索引表的后端快速路径
    if (accountStore_) {
        core::EntityId id = 0;
        std::string password;
        bool found = accountStore_->queryAccount(username, id, password);
        auto resp = DBProtocol::encodeQueryAccountResponse(found, id, password);
        sendResponse(inv.sourceComponent, DBMethod::kQueryAccountOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    // 回退：FileEntityStore 没有索引表，线性扫描 Account 实体
    auto ids = store_->listIdsByType("Account");
    for (auto id : ids) {
        core::EntityData data;
        if (!store_->load(id, "Account", data)) continue;

        // Find "username" property
        for (const auto& prop : data.properties) {
            if (prop.name == "username") {
                std::string storedName(prop.rawValue.begin(), prop.rawValue.end());
                if (storedName == username) {
                    // Found — extract password
                    std::string password;
                    for (const auto& p : data.properties) {
                        if (p.name == "password") {
                            password = std::string(p.rawValue.begin(), p.rawValue.end());
                            break;
                        }
                    }
                    auto resp = DBProtocol::encodeQueryAccountResponse(true, id, password);
                    sendResponse(inv.sourceComponent, DBMethod::kQueryAccountOk,
                                 std::span<const std::byte>(resp.data(), resp.size()));
                    return;
                }
            }
        }
    }

    // Not found
    auto resp = DBProtocol::encodeQueryAccountResponse(false, 0, "");
    sendResponse(inv.sourceComponent, DBMethod::kQueryAccountOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleCreateAccount(const runtime::RuntimeInvocation& inv) {
    ScopedTimer timer([](double ms) {
        dbHistogram("db_create_account_ms", "account create latency in milliseconds").observe(ms);
    });

    std::string username;
    std::string password;
    if (!DBProtocol::decodeCreateAccountRequest(inv.payload, username, password)) {
        auto resp = DBProtocol::encodeCreateAccountResponse(false, 0);
        sendResponse(inv.sourceComponent, DBMethod::kCreateAccountOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    // 优先走索引表后端
    if (accountStore_) {
        core::EntityId id = 0;
        bool ok = accountStore_->createAccount(username, password, id);
        auto resp = DBProtocol::encodeCreateAccountResponse(ok, id);
        sendResponse(inv.sourceComponent, DBMethod::kCreateAccountOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    // 回退：FileEntityStore 线性扫描查重后插入
    // Check if account already exists
    auto ids = store_->listIdsByType("Account");
    for (auto id : ids) {
        core::EntityData data;
        if (!store_->load(id, "Account", data)) continue;
        for (const auto& prop : data.properties) {
            if (prop.name == "username") {
                std::string storedName(prop.rawValue.begin(), prop.rawValue.end());
                if (storedName == username) {
                    auto resp = DBProtocol::encodeCreateAccountResponse(false, 0);
                    sendResponse(inv.sourceComponent, DBMethod::kCreateAccountOk,
                                 std::span<const std::byte>(resp.data(), resp.size()));
                    return;
                }
            }
        }
    }

    // Create new account
    auto newId = store_->allocId();
    core::EntityData data;
    data.id = newId;
    data.entityType = "Account";

    core::PropertyData usernameProp;
    usernameProp.id = 0;
    usernameProp.name = "username";
    usernameProp.type = core::DataType::String;
    usernameProp.rawValue.assign(
        reinterpret_cast<const std::byte*>(username.data()),
        reinterpret_cast<const std::byte*>(username.data()) + username.size());
    data.properties.push_back(usernameProp);

    core::PropertyData passwordProp;
    passwordProp.id = 1;
    passwordProp.name = "password";
    passwordProp.type = core::DataType::String;
    passwordProp.rawValue.assign(
        reinterpret_cast<const std::byte*>(password.data()),
        reinterpret_cast<const std::byte*>(password.data()) + password.size());
    data.properties.push_back(passwordProp);

    bool ok = store_->save(newId, data);
    auto resp = DBProtocol::encodeCreateAccountResponse(ok, newId);
    sendResponse(inv.sourceComponent, DBMethod::kCreateAccountOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::sendResponse(runtime::ComponentId target,
                          const std::string& method,
                          std::span<const std::byte> payload) {
    runtime::RuntimeInvocation resp;
    resp.sourceComponent = config_.componentId;
    resp.targetComponent = target;
    resp.entityId = 0;
    resp.method = method;
    resp.payload = std::vector<std::byte>(payload.begin(), payload.end());
    hub_->send(std::move(resp));
    hub_->flush();
}

}  // namespace theseed::db
