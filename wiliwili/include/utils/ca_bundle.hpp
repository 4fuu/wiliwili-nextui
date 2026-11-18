//
// Runtime CA bundle lookup for NextUI devices without /etc/ssl.
//

#pragma once

#include <string>

namespace wiliwili {

/**
 * Returns a readable CA bundle path, or an empty string when none was found
 * (callers should then leave the library's compiled-in default in place).
 *
 * Order: CURL_CA_BUNDLE, SSL_CERT_FILE, the NextUI system bundle, then the
 * pak's own res/ca-bundle.crt.
 */
const std::string& getCaBundlePath();

}  // namespace wiliwili
