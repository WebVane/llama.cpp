#include "license/Ed25519.h"

#include <openssl/evp.h>

namespace visio::license {

bool ed25519_verify(const uint8_t* pubkey, const uint8_t* signature,
                    const uint8_t* msg, std::size_t msg_len) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, pubkey, 32);
    if (!pkey) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
        // Ed25519 is "one-shot" via EVP_DigestVerify (not …Update + …Final).
        const int rv = EVP_DigestVerify(ctx, signature, 64, msg, msg_len);
        ok = (rv == 1);
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

}  // namespace visio::license
