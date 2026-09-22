#include "theseed/core/CellApp.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using theseed::core::CellApp;
using theseed::foundation::MetricsRegistry;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::Vector3;

#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                       \
    do {                                                \
        std::cout << "FAILED: " << msg << std::endl;    \
        return 1;                                       \
    } while (0)

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// 创建临时 def 目录，与 CellAppTest 同结构。
static std::string createDefDir() {
    std::string dir = "test_cellapp_obs_defs";
    std::filesystem::create_directories(dir);
    writeFile(dir + "/Avatar.xml",
        "<EntityDef name=\"Avatar\">"
        "  <Properties><Property name=\"level\" type=\"Int32\"/></Properties>"
        "</EntityDef>");
    return dir;
}

static bool metricContains(const std::string& text, const std::string& metricName) {
    return text.find(metricName) != std::string::npos;
}

int main() {
    std::cout << "CellApp observability integration tests:" << std::endl;

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = theseed::runtime::ComponentId{2};
    config.ops.enabled = false;  // 测试不启 HTTP 端口

    CellApp app(std::move(config), transport);
    if (!app.init()) {
        std::filesystem::remove_all(dir);
        FAIL("CellApp init failed");
    }

    app.createEntity("Avatar", Vector3{1, 0, 0});
    app.createEntity("Avatar", Vector3{2, 0, 0});

    std::cout << "  entity_count gauge updates after tick... ";
    app.tick();  // 刷新 entity_count gauge
    {
        auto text = MetricsRegistry::instance().renderText();
        if (!metricContains(text, "entity_count")) {
            std::filesystem::remove_all(dir);
            FAIL("entity_count not in metrics");
        }
    }
    PASS();

    std::cout << "  transport stats (queue_backlog/backpressure) registered... ";
    {
        auto text = MetricsRegistry::instance().renderText();
        bool ok = metricContains(text, "queue_backlog") &&
                  metricContains(text, "transport_backpressure");
        if (!ok) {
            std::filesystem::remove_all(dir);
            FAIL("transport stats metrics missing");
        }
    }
    PASS();

    std::filesystem::remove_all(dir);

    std::cout << "\nCellApp observability: all passed\n";
    return 0;
}
