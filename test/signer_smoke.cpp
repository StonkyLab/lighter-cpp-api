/**
Lighter signer link/load smoke: generate an API key pair via the vendored
native signer. No credentials or network needed — proves the .so loads and the
cgo C ABI resolves. The private key is deliberately not printed because CI and
terminal logs are not a secret transport.
*/

#include "stonky/lighter/lighter_signer.h"
#include <spdlog/spdlog.h>

int main() {
    try {
        const auto key = stonky::lighter::generateApiKey();
        spdlog::info("Lighter signer OK — generated API key pair");
        spdlog::info("  publicKey  = {}", key.publicKey);
        spdlog::info("  private key generated successfully (redacted)");
    } catch (std::exception &e) {
        spdlog::critical("signer smoke failed: {}", e.what());
        return 1;
    }

    return 0;
}
