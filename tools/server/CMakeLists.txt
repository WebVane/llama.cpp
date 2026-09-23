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
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

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

# Trial / evaluation build: limit the number of inference requests served
# (web UI and API alike). Health checks and metadata endpoints are exempt.
# OFF by default so a normal build is unrestricted. The gate lives in
# server.cpp, which is part of llama-server-impl, so the definitions and the
# Trial sources are attached to that library target.
option(LLAMA_SERVER_TRIAL "Build provision-server as a request-limited trial" OFF)
set(LLAMA_SERVER_TRIAL_LIMIT 50 CACHE STRING "Trial request limit")
set(LLAMA_SERVER_TRIAL_KEY "" CACHE STRING "Override the HMAC key used to sign trial state")

if (LLAMA_SERVER_TRIAL)
    target_sources(llama-server-impl PRIVATE ${PROJECT_SOURCE_DIR}/src/license/Trial.cpp)
    target_include_directories(llama-server-impl PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(llama-server-impl PRIVATE
        VISIO_TRIAL=1
        VISIO_TRIAL_LIMIT=${LLAMA_SERVER_TRIAL_LIMIT})
    if (NOT LLAMA_SERVER_TRIAL_KEY STREQUAL "")
        target_compile_definitions(llama-server-impl PRIVATE VISIO_TRIAL_KEY="${LLAMA_SERVER_TRIAL_KEY}")
    endif()

    # The trial counter signs its state with HMAC-SHA256, which needs libcrypto.
    # Reuse whatever crypto provider the build already has: the system OpenSSL
    # (OpenSSL::Crypto), or the bundled BoringSSL/LibreSSL pulled in by
    # cpp-httplib (the `crypto` target). Only fall back to a hard requirement
    # if neither is present.
    if (TARGET OpenSSL::Crypto)
        target_link_libraries(llama-server-impl PRIVATE OpenSSL::Crypto)
    elseif (TARGET crypto)
        target_link_libraries(llama-server-impl PRIVATE crypto)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(llama-server-impl PRIVATE OpenSSL::Crypto)
    endif()

    message(STATUS "provision-server: TRIAL build enabled, request limit = ${LLAMA_SERVER_TRIAL_LIMIT}")
endif()
