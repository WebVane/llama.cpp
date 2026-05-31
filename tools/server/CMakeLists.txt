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
    server-tools.cpp
    server-tools.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})


# llama-server executable

set(TARGET llama-server)

set(TARGET_SRCS
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)

option(LLAMA_BUILD_WEBUI "Build the embedded Web UI" ON)

if (LLAMA_BUILD_WEBUI)
    set(PUBLIC_ASSETS
        index.html
        bundle.js
        bundle.css
        loading.html
        favicon.svg
        swagger.html
        swagger-ui.css
        swagger-ui-bundle.js
        openapi.json
    )

    foreach(asset ${PUBLIC_ASSETS})
        set(input "${CMAKE_CURRENT_SOURCE_DIR}/public/${asset}")
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${asset}.hpp")
        list(APPEND TARGET_SRCS ${output})
        add_custom_command(
            DEPENDS "${input}"
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DOUTPUT=${output}" -P "${PROJECT_SOURCE_DIR}/scripts/xxd.cmake"
        )
        set_source_files_properties(${output} PROPERTIES GENERATED TRUE)
    endforeach()
    add_definitions(-DLLAMA_BUILD_WEBUI)
else()
endif()

add_executable(${TARGET} ${TARGET_SRCS})
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME provision-server)
install(TARGETS ${TARGET} RUNTIME)

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PRIVATE server-context PUBLIC llama-common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

target_compile_features(${TARGET} PRIVATE cxx_std_17)

# Trial / evaluation build: limit the number of inference requests served
# (web UI and API alike). Health checks and metadata endpoints are exempt.
# OFF by default so a normal build is unrestricted.
option(LLAMA_SERVER_TRIAL "Build provision-server as a request-limited trial" OFF)
set(LLAMA_SERVER_TRIAL_LIMIT 50 CACHE STRING "Trial request limit")
set(LLAMA_SERVER_TRIAL_KEY "" CACHE STRING "Override the HMAC key used to sign trial state")

if (LLAMA_SERVER_TRIAL)
    target_sources(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/license/Trial.cpp)
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${TARGET} PRIVATE
        VISIO_TRIAL=1
        VISIO_TRIAL_LIMIT=${LLAMA_SERVER_TRIAL_LIMIT})
    if (NOT LLAMA_SERVER_TRIAL_KEY STREQUAL "")
        target_compile_definitions(${TARGET} PRIVATE VISIO_TRIAL_KEY="${LLAMA_SERVER_TRIAL_KEY}")
    endif()

    # The trial counter signs its state with HMAC-SHA256, which needs libcrypto.
    # Reuse whatever crypto provider the build already has: the system OpenSSL
    # (OpenSSL::Crypto), or the bundled BoringSSL/LibreSSL pulled in by
    # cpp-httplib (the `crypto` target). Only fall back to a hard requirement
    # if neither is present.
    if (TARGET OpenSSL::Crypto)
        target_link_libraries(${TARGET} PRIVATE OpenSSL::Crypto)
    elseif (TARGET crypto)
        target_link_libraries(${TARGET} PRIVATE crypto)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(${TARGET} PRIVATE OpenSSL::Crypto)
    endif()

    message(STATUS "provision-server: TRIAL build enabled, request limit = ${LLAMA_SERVER_TRIAL_LIMIT}")
endif()
