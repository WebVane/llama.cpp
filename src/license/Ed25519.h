// Ed25519 signature verification, OpenSSL EVP backend
// (EVP_PKEY_ED25519 + EVP_DigestVerify).  Pure verify — the client never
// signs, so no keygen / sign primitives are exposed.
//
// Inputs are raw bytes:
//   * pubkey: 32 bytes (RFC 8032 §5.1.5 "A" — the encoded public key)
//   * signature: 64 bytes (RFC 8032 §5.1.6 "R ‖ S")
//   * message: any length
//
// Returns true iff the signature is a valid Ed25519-pure signature of the
// message under the given public key.  Returns false on any failure mode
// (malformed key, malformed signature, OpenSSL allocation failure,
// algorithm-level rejection) — failure modes are not distinguished because
// the caller only ever turns this into accept-or-reject.
#pragma once

#include <cstddef>
#include <cstdint>

namespace visio::license {

bool ed25519_verify(const uint8_t* pubkey, const uint8_t* signature,
                    const uint8_t* msg, std::size_t msg_len);

}  // namespace visio::license
