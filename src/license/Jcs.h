// Canonical JSON (RFC 8785 / JCS) serializer, with one documented
// extension: DEL (U+007F) is escaped as .  The license server uses
// the same extension (see src/license/docs/vectors/golden/metadata.json
// and README §1) — any drift here breaks signature verification.
//
// Rules implemented (verbatim from src/license/docs/vectors/golden/README.md):
//   * Object keys sorted ascending by UTF-16 code units (uppercase < lowercase;
//     for ASCII this coincides with byte order).
//   * Compact separators "," and ":" — no whitespace anywhere.
//   * Strings: escape `"`, `\`, short forms \b \t \n \f \r for 0x08 0x09 0x0A
//     0x0C 0x0D; other C0 controls (<0x20) AND DEL (0x7F) → \u00XX lowercase
//     hex; everything else emitted as literal UTF-8 (including U+2028/U+2029).
//   * Numbers: integers only (license schema constraint).  Emitted via
//     std::to_string of the signed/unsigned 64-bit value.  Doubles that are
//     not integer-valued cause a runtime error — they should never appear
//     in a valid .lic.
//   * Booleans/null as literal `true`, `false`, `null`.
//   * Arrays preserve input order; we do not sort them.
//   * UTF-8 encoding, no BOM.
//
// Absent vs null: the input is an nlohmann::json tree, so this serializer
// trusts the caller's distinction — a key not present in the object is
// not emitted; a key with value `json::value_t::null` emits the literal
// `null`.  This matters for `fingerprint` (absent ↔ offline) and
// `limits.requestLimit` (absent ↔ unlimited).
//
// Failure mode: returns the canonical string on success; on a
// schema-invalid input (non-integer number, non-UTF-8 string) the function
// throws std::runtime_error.  The caller must wrap and translate that to
// a license-validation failure (bad payload), not let it crash the process.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace visio::license {

std::string jcs_canonicalize(const nlohmann::json& v);

}  // namespace visio::license
