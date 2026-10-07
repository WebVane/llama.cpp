#pragma once

//
// Provision admin layer (fork-owned, see docs/provision-admin.md).
//
// Adds the /admin/* HTTP API to the ROUTER process: runtime settings
// (models_max, default device for new models, pinned models), per-model
// settings (GPU, context size, ...) with a dry-run fit check, a GPU memory
// view, the output tail of failed model starts, diagnostics, storage
// information and a pre-flight check for Hugging Face downloads. It never
// touches the existing routes. Everything is inert unless PROVISION_ADMIN is
// enabled in the environment of the router. An optional address allow-list
// (PROVISION_ADMIN_ALLOW_FROM, off by default) guards every /admin route; it
// reads the client address from server_http_req::remote_addr.
//
// The upstream files only carry small call-outs into this layer, each tagged
// with a "provision-admin" comment (grep for it after merging upstream).
//
// Locking: the layer owns one outer mutex (settings changes) and a few leaf
// mutexes (see server-admin.cpp). The snapshot read by
// server_admin_overlay_presets() is guarded by a leaf mutex that is never held
// while calling into server_models, so the hook is safe to call from
// server_models::load_models() - including the first call made by the
// server_models constructor and the lazy reload triggered by get_meta().
// server_models never calls into this layer while holding its own mutex,
// except for server_admin_overlay_changed(), which takes no lock at all, and
// server_admin_child_line(), which takes only a leaf mutex.
//

#include "server-models.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>

struct server_http_context;

// Read the PROVISION_ADMIN* environment, remove those variables from the
// process environment (child processes must never see the admin key), resolve
// the state directory and load the saved settings. When the saved settings
// carry models_max, params.models_max is overridden.
// Call it in the router process only, BEFORE server_models is constructed
// (the constructor snapshots the environment for the child processes).
// No-op in a router child.
void server_admin_init(common_params & params);

// true when the admin layer is enabled in this process
bool server_admin_enabled();

// Register the /admin/* routes and push the saved settings (pinned models)
// into server_models. Call it after the existing router routes are
// registered. No-op unless the admin layer is enabled.
void server_admin_register_routes(server_http_context & ctx_http, server_models_routes & routes);

// Hook for server_models::load_models(), to be called between the merge of
// the model sources and the overlay of the router's own CLI args
// (base_preset, merged by the caller right after this returns):
//   - the per-model settings saved through /admin/models/settings are applied
//     (a key the router command line or MODEL_DEVICES sets is skipped: those win);
//   - models that do not come from the preset file (source cache / models_dir)
//     and still have no device get the admin "GPU for new models" device.
// The values each model had before (models.ini, [*]) are recorded for the
// admin API. Never calls back into server_models and never throws.
// No-op unless the admin layer is enabled.
// Returns the generation of the settings it applied, for
// server_admin_overlay_changed().
uint64_t server_admin_overlay_presets(
        const common_preset_context & ctx_preset,
        common_presets & presets,
        const std::unordered_map<std::string, server_model_source> & source_map,
        const common_preset & base_preset);

// true when the settings the hook applies changed after server_admin_overlay_presets()
// returned `generation`: the presets built from it are outdated. A reload can
// run concurrently with the one a settings change starts and finish after it,
// so server_models::load_models() checks this before it completes.
// Lock-free, safe to call while holding the server_models mutex. Always false
// when the admin layer is disabled.
bool server_admin_overlay_changed(uint64_t generation);

// Call-out for every plain log line a router child writes (never for the
// cmd_child_to_router:state: lines), from the monitor thread of
// server_models: keeps a bounded in-memory tail per model for
// /admin/models/log. `instance` identifies the child process (a new value
// starts a new tail). Takes only a leaf mutex, never calls into
// server_models. No-op unless the admin layer is enabled.
void server_admin_child_line(const std::string & name, const void * instance, int port, const std::string & line);

// Helper modes of the server binary used by the admin layer (device
// inventory, dry-run fit check): the router starts its own binary with
// PROVISION_ADMIN_HELPER set, so that GPU contexts are only ever created in
// a short-lived process and never in the router. Call it first thing in the
// server's main: returns true when this process is such a helper, which has
// then done its work and must exit with `exit_code`; false otherwise
// (nothing happened).
bool server_admin_helper_main(int argc, char ** argv, int & exit_code);

// Typed pre-check of a projector (mmproj) before server_model_meta::update_caps() reads its capabilities:
// the clip loader reads a few keys with typed getters that abort the whole process for another type (a damaged
// or hand-made file in --models-dir crashed the router on every reload). false: the file is not used for
// multimodal input (logged). Works with the admin layer disabled too; reads only the GGUF header, bounded.
bool server_admin_mmproj_usable(const std::string & path);

// Runs fn only when the enclosing scope is left by an exception.
// Used by server_models::load_models() to reset is_reloading when a reload throws.
struct server_admin_on_throw {
    explicit server_admin_on_throw(std::function<void()> fn)
        : fn(std::move(fn)), n_exceptions(std::uncaught_exceptions()) {}

    ~server_admin_on_throw() {
        if (std::uncaught_exceptions() > n_exceptions) {
            try {
                fn();
            } catch (...) {
                // already unwinding, nothing more to do
            }
        }
    }

    server_admin_on_throw(const server_admin_on_throw &) = delete;
    server_admin_on_throw & operator=(const server_admin_on_throw &) = delete;

private:
    std::function<void()> fn;
    int n_exceptions;
};
