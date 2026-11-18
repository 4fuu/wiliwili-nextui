//
// Runtime CA bundle lookup for NextUI devices without /etc/ssl.
//

#include "utils/ca_bundle.hpp"

#include <cstdlib>
#include <fstream>

#include <borealis/core/logger.hpp>

namespace wiliwili {

namespace {

bool readableFile(const char* path) {
    if (!path || path[0] == '\0') return false;
    std::ifstream file(path);
    return file.good();
}

}  // namespace

const std::string& getCaBundlePath() {
    static const std::string path = []() -> std::string {
        const char* envPaths[] = {std::getenv("CURL_CA_BUNDLE"), std::getenv("SSL_CERT_FILE")};
        for (const char* path : envPaths) {
            if (readableFile(path)) {
                brls::Logger::info("CA bundle from environment: {}", path);
                return path;
            }
        }

        // NextUI's system bundle, then the copy shipped in the pak (the launch
        // script changes into the pak root before starting the binary).
        const char* fallbackPaths[] = {"/mnt/SDCARD/.system/res/ca-bundle.crt", "res/ca-bundle.crt"};
        for (const char* path : fallbackPaths) {
            if (readableFile(path)) {
                brls::Logger::info("CA bundle from device: {}", path);
                return path;
            }
        }

        brls::Logger::info("No CA bundle found, using library defaults");
        return {};
    }();
    return path;
}

}  // namespace wiliwili
