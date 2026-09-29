# `.lic` canonicalization & golden vectors (C++ client contract)

Source of truth for stitching the C++ client (LicenseLib) to the License Server. Everything signed
is signed over **canonical bytes**; any byte-level drift breaks signature verification on the other side.

## 1. Canonicalization — RFC 8785 (JCS), with one documented extension

Implemented by `CanonicalJson` (`license-server/.../service/cripto/CanonicalJson.java`).

- **Object keys**: sorted ascending by **UTF-16 code units** (Java `String` natural order; for ASCII this
  equals codepoint and UTF-8 byte order). Uppercase sorts before lowercase.
- **Separators**: compact — `,` and `:`, no whitespace.
- **Strings** (RFC 8785 §3.2.2.2):
  - `"` → `\"`, `\` → `\\`
  - short forms `\b \t \n \f \r` for `0x08 0x09 0x0A 0x0C 0x0D`
  - other C0 controls (`< 0x20`) **and DEL (`0x7F`)** → `\u00xx` with **lowercase** hex
  - everything else — including non-ASCII and `U+2028`/`U+2029` — emitted **literally** as UTF-8
  - **Extension vs pure JCS**: pure ECMAScript/JCS leaves `0x7F` literal; we escape it as ``.
    The C++ side **must** escape DEL the same way. (Real `.lic` payloads contain no control chars,
    so this never affects the vectors below — it only fixes adversarial/control-char inputs.)
- **Numbers**: integers via ECMAScript `Number.toString` — no decimal point, no leading zeros.
  Signed `.lic` payloads contain **only integers** (`maxServers`, `limits.requestLimit`); non-integers are
  not used in `.lic`.
- `true` / `false` / `null` — literals.
- Encoding: **UTF-8**.

## 2. Date/time (`issuedAt`, `expiresAt`)

**RFC 3339, always `Z` (UTC), seconds precision** (no fractional seconds), e.g. `2026-07-01T00:00:00Z`.
`.lic.expiresAt` = the license `end_date` (fixed at purchase/renewal), not `now + term`.

## 3. `limits.requestLimit` and `fingerprint`

- `limits.requestLimit` when there is **no limit**: the field is **omitted** (absent), never `null`.
- `fingerprint` is present **only for Online**; **Offline `.lic` never contains `fingerprint`** — its
  presence/absence is the single format difference between Online and Offline.

## 4. `.lic` shape

```
{ "licenseId", "clientId", "product", "tariff", "activationCode",
  "fingerprint"(online only), "maxServers", "limits": { "requestLimit"(opt), "gpu" },
  "issuedAt", "expiresAt", "kid", "signature" }
```

## 5. Verification algorithm (client)

1. Parse the `.lic` JSON.
2. Remove the `signature` field; **canonicalize** the remaining object (rules above) → payload bytes.
3. Look up the public key by `kid` in the keyring (`docs/keyring.json`, accept-if-any).
4. `Ed25519.verify(pubkey, payloadBytes, base64url_decode(signature))`.
5. Additionally check: `fingerprint` matches the local machine (online), `expiresAt` not passed, and (if
   present) local request counter `< limits.requestLimit`.

`signature` is **base64url** (no padding) of the 64-byte Ed25519 signature. `pubkey` is **base64url** of the
raw 32-byte Ed25519 public key.

## 6. Files in this directory

| File | Content |
|---|---|
| `online.lic` | full signed Online `.lic` (canonical bytes, **with** `fingerprint`) |
| `online.payload.json` | canonical payload that was signed (Online, `signature` removed) |
| `offline.lic` | full signed Offline `.lic` (canonical bytes, **no** `fingerprint`) |
| `offline.payload.json` | canonical payload that was signed (Offline) |
| `metadata.json` | algorithm, `kid`, public key (base64url raw 32), and both signatures |
| `../keyring.json` | `{ "keys": [ { "kid", "pubkey" } ] }` (public keys only) |

A C++ implementation is correct iff: re-canonicalizing `*.payload.json`'s parsed object reproduces the file
byte-for-byte, **and** Ed25519-verify of that payload against `metadata.publicKey` with the listed `signature`
succeeds.

> **Test key only.** The keypair in these vectors is the fixed test key from `CanonicalCryptoTest` — **not a
> production key**. Production `.lic` signing keys live in Vault/KMS; the LS publishes only public keys + `kid`.
