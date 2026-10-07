include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
    # provision-admin: fork-owned admin layer (/admin/* routes)
    server-admin.cpp
    server-admin.h
    server-admin-internal.h
    server-admin-upload.cpp
    server-admin-upload.h
    server-admin-audit.cpp
    server-admin-audit.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(${TARGET} PRIVATE vendor::hash) # provision-admin: SHA-256 of uploaded files

add_dependencies(${TARGET} llama-ui-assets)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME provision-server)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

# ---------------------------------------------------------------------------
# License subsystem (src/license, shared with provision_ocr_server): Ed25519
# signed .lic files, online activation against the License Server and an
# HMAC-signed request counter. Gates the inference routes in server.cpp (see
# license_gate there); /health, /props, /models, tokenize and the UI assets
# stay free. OFF by default so a plain dev build is unrestricted.
option(LLAMA_SERVER_LICENSE "Build the license subsystem into provision-server and gate inference requests" OFF)
# Trial fallback: with no .lic installed the server runs in trial mode and
# serves LLAMA_SERVER_TRIAL_LIMIT inference requests per install, then locks
# and asks for activation. Implies LLAMA_SERVER_LICENSE.
option(LLAMA_SERVER_TRIAL "Trial fallback: N free requests per install before a license is required" OFF)
set(LLAMA_SERVER_TRIAL_LIMIT 50 CACHE STRING "Trial request limit")
# Product name a .lic must carry ("LLM" on the License Server) and the
# server it is obtained from; both can still be overridden at runtime via
# PROVISION_LICENSE_PRODUCT / PROVISION_LICENSE_ACTIVATION_URL.
set(LLAMA_SERVER_LICENSE_PRODUCT "LLM" CACHE STRING "License Server product name a .lic must match")
set(LLAMA_SERVER_LICENSE_ACTIVATION_URL "https://license.scan-dev.com" CACHE STRING "License Server base URL for online activation")
# Trusted Ed25519 public keys, baked into the binary at configure time
# ({ "keys": [ { "kid": "...", "pubkey": "<base64url 32B>" }, ... ] }).
# Empty = no .lic can be verified (secure default for dev builds); CI
# provides the production keyring as a secret file.
set(LLAMA_SERVER_LICENSE_KEYRING_JSON "" CACHE FILEPATH "Keyring JSON to bake in (production builds)")
set(LLAMA_SERVER_LICENSE_COUNTER_KEY "" CACHE STRING "Override the HMAC key that signs the request counter files")
set(LLAMA_SERVER_TRIAL_KEY "" CACHE STRING "Deprecated alias of LLAMA_SERVER_LICENSE_COUNTER_KEY")

if (LLAMA_SERVER_LICENSE OR LLAMA_SERVER_TRIAL)
    set(LICENSE_DIR ${PROJECT_SOURCE_DIR}/src/license)
    if (NOT EXISTS ${LICENSE_DIR}/LicenseLib.cpp)
        message(FATAL_ERROR "LLAMA_SERVER_LICENSE/TRIAL: src/license module not found (LicenseLib.cpp missing)")
    endif()

    add_library(provision-license STATIC
        # crypto + parsing core
        ${LICENSE_DIR}/Base64Url.cpp
        ${LICENSE_DIR}/Sha1.cpp
        ${LICENSE_DIR}/Sha256.cpp
        ${LICENSE_DIR}/Ed25519.cpp
        ${LICENSE_DIR}/Jcs.cpp
        ${LICENSE_DIR}/Keyring.cpp
        ${LICENSE_DIR}/License.cpp
        ${LICENSE_DIR}/Uuid.cpp
        # online activation client + machine fingerprint + HTTP transport
        ${LICENSE_DIR}/Fingerprint.cpp
        ${LICENSE_DIR}/Http.cpp
        ${LICENSE_DIR}/Activation.cpp
        # facade: mode resolution, request counter, in-process cache
        ${LICENSE_DIR}/RequestCounter.cpp
        ${LICENSE_DIR}/LicenseLib.cpp
    )
    target_include_directories(provision-license PUBLIC ${PROJECT_SOURCE_DIR}/src)
    # llama-common for log.h, cpp-httplib for the activation transport (and
    # its TLS / libcrypto provider), nlohmann for the .lic parser
    target_link_libraries(provision-license PUBLIC llama-common cpp-httplib vendor::nlohmann)
    target_compile_features(provision-license PUBLIC cxx_std_17)
    if (BUILD_SHARED_LIBS)
        set_target_properties(provision-license PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    # libcrypto for SHA-256 / HMAC / Ed25519: reuse whatever provider the build
    # already has — the system OpenSSL (LLAMA_OPENSSL) or the bundled
    # BoringSSL / LibreSSL pulled in by cpp-httplib (the `crypto` target).
    if (TARGET OpenSSL::Crypto)
        target_link_libraries(provision-license PUBLIC OpenSSL::Crypto)
    elseif (TARGET crypto)
        target_link_libraries(provision-license PUBLIC crypto)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(provision-license PUBLIC OpenSSL::Crypto)
    endif()
    get_target_property(_httplib_defs cpp-httplib INTERFACE_COMPILE_DEFINITIONS)
    if (NOT _httplib_defs MATCHES "CPPHTTPLIB_OPENSSL_SUPPORT")
        message(WARNING "provision-license: cpp-httplib has no TLS support, so an https:// activation URL "
                        "will fail at runtime. Build with LLAMA_OPENSSL=ON or LLAMA_BUILD_BORINGSSL=ON.")
    endif()
    if (WIN32)
        # Fingerprint reads MachineGuid from the registry (advapi32) and the
        # hostname via gethostname (ws2_32).
        target_link_libraries(provision-license PUBLIC advapi32 ws2_32)
    endif()

    target_compile_definitions(provision-license PUBLIC
        PROVISION_LICENSE=1
        "PROVISION_LICENSE_PRODUCT=\"${LLAMA_SERVER_LICENSE_PRODUCT}\""
        "PROVISION_LICENSE_ACTIVATION_URL=\"${LLAMA_SERVER_LICENSE_ACTIVATION_URL}\"")
    if (LLAMA_SERVER_TRIAL)
        target_compile_definitions(provision-license PUBLIC
            PROVISION_TRIAL=1
            PROVISION_TRIAL_LIMIT=${LLAMA_SERVER_TRIAL_LIMIT})
    endif()
    if (NOT LLAMA_SERVER_LICENSE_COUNTER_KEY STREQUAL "")
        target_compile_definitions(provision-license PRIVATE
            "PROVISION_LICENSE_COUNTER_KEY=\"${LLAMA_SERVER_LICENSE_COUNTER_KEY}\"")
    elseif (NOT LLAMA_SERVER_TRIAL_KEY STREQUAL "")
        target_compile_definitions(provision-license PRIVATE
            "PROVISION_LICENSE_COUNTER_KEY=\"${LLAMA_SERVER_TRIAL_KEY}\"")
    endif()

    # Production keyring: the JSON bytes are inlined into keyring_baked.cpp via
    # configure_file (read ONCE at configure time, so rolling keys needs a
    # rebuild). Without it Keyring::baked() in Keyring.cpp returns an empty
    # keyring and every .lic fails verification.
    if (LLAMA_SERVER_LICENSE_KEYRING_JSON)
        if (NOT EXISTS "${LLAMA_SERVER_LICENSE_KEYRING_JSON}")
            message(FATAL_ERROR "LLAMA_SERVER_LICENSE_KEYRING_JSON='${LLAMA_SERVER_LICENSE_KEYRING_JSON}' does not exist")
        endif()
        file(READ "${LLAMA_SERVER_LICENSE_KEYRING_JSON}" PROVISION_LICENSE_KEYRING_JSON_BYTES)
        # Refuse a keyring the runtime could not use: a build with an empty or
        # unparsable keyring starts fine but can never verify a .lic, which
        # only shows up as "unknown_kid" on a customer's activation.
        if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
            string(JSON _KEYRING_N_KEYS ERROR_VARIABLE _KEYRING_ERR LENGTH "${PROVISION_LICENSE_KEYRING_JSON_BYTES}" keys)
            if (NOT _KEYRING_ERR STREQUAL "NOTFOUND")
                message(FATAL_ERROR "LLAMA_SERVER_LICENSE_KEYRING_JSON is not a keyring JSON ({\"keys\":[{\"kid\",\"pubkey\"},...]}): ${_KEYRING_ERR}")
            endif()
            if (_KEYRING_N_KEYS EQUAL 0)
                message(FATAL_ERROR "LLAMA_SERVER_LICENSE_KEYRING_JSON holds no keys")
            endif()
        elseif (NOT PROVISION_LICENSE_KEYRING_JSON_BYTES MATCHES "\"keys\"")
            message(FATAL_ERROR "LLAMA_SERVER_LICENSE_KEYRING_JSON does not look like a keyring JSON (no \"keys\")")
        endif()
        string(FIND "${PROVISION_LICENSE_KEYRING_JSON_BYTES}" ")VISIOLIC_BAKED" _RAW_DELIM_HIT)
        if (NOT _RAW_DELIM_HIT EQUAL -1)
            message(FATAL_ERROR "Keyring JSON contains the reserved raw-string delimiter ')VISIOLIC_BAKED' - refusing to inline")
        endif()
        set(_BAKED_OUT "${CMAKE_BINARY_DIR}/gen/license/keyring_baked.cpp")
        configure_file("${LICENSE_DIR}/keyring_baked.cpp.in" "${_BAKED_OUT}" @ONLY)
        target_sources(provision-license PRIVATE "${_BAKED_OUT}")
        target_compile_definitions(provision-license PRIVATE PROVISION_LICENSE_HAS_BAKED_KEYRING=1)
        message(STATUS "provision-license: baked keyring from ${LLAMA_SERVER_LICENSE_KEYRING_JSON}")
    else()
        message(STATUS "provision-license: LLAMA_SERVER_LICENSE_KEYRING_JSON not set - keyring EMPTY, no .lic can be verified")
    endif()

    target_link_libraries(llama-server-impl PRIVATE provision-license)

    if (LLAMA_SERVER_TRIAL)
        message(STATUS "provision-server: license subsystem ON, trial fallback = ${LLAMA_SERVER_TRIAL_LIMIT} requests, product = ${LLAMA_SERVER_LICENSE_PRODUCT}")
    else()
        message(STATUS "provision-server: license subsystem ON (no trial fallback), product = ${LLAMA_SERVER_LICENSE_PRODUCT}")
    endif()
endif()
