#pragma once

//
// Provision admin layer: what server-admin.cpp shares with the other fork-owned admin files
// (server-admin-upload.cpp). Not an API of the server: only the fork-owned admin files include it.
//
// Everything here is implemented in server-admin.cpp on top of its own (static) helpers, so that the
// admin_context stays private to that file. The functions may only be called while the admin layer is
// enabled (server_admin_enabled()), after server_admin_register_routes() set the routes, unless their
// comment says otherwise.
//

#include "server-common.h"
#include "server-http.h"
#include "server-models.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// an error that maps to a specific HTTP status and error.type (the stage-1 error format,
// {"error": {"code": status, "message": ..., "type": type, "param"?: ..., <extra fields>}})
struct admin_http_error : std::runtime_error {
    int         status;
    std::string type;
    std::string param; // the request field it is about, "" = none
    json        extra; // more fields for the error object
    admin_http_error(int status, const std::string & type, const std::string & message)
        : std::runtime_error(message), status(status), type(type) {}
};

using admin_kv = std::map<std::string, std::string>; // INI key -> value (canonical spelling for stored settings)

//
// HTTP
//

using admin_route_fn = std::function<server_http_res_ptr(const server_http_req & req)>;

// the handler of a /admin route that needs the admin key: allow-list, key, then fn; every exception
// becomes an error response (admin_http_error keeps its status)
server_http_context::handler_t admin_internal_wrap(admin_route_fn fn);

// the same allow-list and key checks without a handler: nullptr = allowed, else the refusal
// (used before a request body is read, see server_http_context::set_pre_body_handler)
server_http_res_ptr admin_internal_refusal(const server_http_req & req);

server_http_res_ptr admin_internal_json(int status, const json & data);
server_http_res_ptr admin_internal_error(int status, const std::string & type, const std::string & message,
                                         const std::string & param = "", const json & extra = json());

// the request body as a JSON object; throws a 400 admin_http_error otherwise
json admin_internal_body_object(const server_http_req & req);

//
// context
//

server_models_routes * admin_internal_routes();
std::filesystem::path  admin_internal_state_dir();
std::mutex &           admin_internal_apply_mutex(); // serialises changes (settings, publish, delete)
bool                   admin_internal_stopping();    // the background threads are being stopped

//
// helper processes (see the comment above admin_job_start in server-admin.cpp)
//

struct admin_job {
    std::string label;
    int64_t     t_start = 0; // steady ms
    bool        ignore_terminate = false; // canned output only: simulates a process that does not die
    std::mutex              mtx;
    std::condition_variable cv;
    bool        done      = false; // the fields below are final
    int64_t     t_done    = 0;     // steady ms when done was set
    bool        spawned   = false;
    bool        timed_out = false;
    int         exit_code = -1;
    std::string output;            // stdout + stderr
    std::atomic<bool> alive{true}; // the process has not been reaped yet
};

std::shared_ptr<admin_job> admin_job_start(
        const std::string & label,
        const std::vector<std::vector<std::string>> & commands,
        const std::vector<std::string> & env,
        int timeout_ms,
        bool ignore_terminate = false);
bool admin_job_wait(const std::shared_ptr<admin_job> & job, int64_t wait_ms);
bool admin_job_is_done(const std::shared_ptr<admin_job> & job);

json                     admin_result_line(const std::string & output);
std::vector<std::string> admin_output_tail(const std::string & output, size_t n);
void                     admin_print_result(const json & result); // in a helper process: the one result line

std::vector<std::string> admin_internal_self_command();                   // this binary (+ LLAMA_APP_CMD)
std::vector<std::string> admin_internal_helper_env(const std::string & mode); // the children's environment + the helper mode

// GGUF facts as GET /admin/models/settings shows them (in a helper process: the router reads only files the helper passed)
json admin_internal_read_facts(const std::string & path, const std::string & mmproj);
void admin_internal_facts_enqueue(const std::string & path, const std::string & mmproj);

//
// per-model settings
//

// Validates the per-model settings of a models_dir model that is not in the model list yet (an upload):
// the same rules as POST /admin/models/settings, including the locks of the router command line and of
// MODEL_DEVICES (the inherited values are taken from the [*] section of the preset file, which a new
// models_dir model gets). `base` are settings kept from an earlier entry (settings_mode keep), merged first.
// Returns the canonical settings; throws admin_http_error (400 invalid_request_error with param, 409 field_locked).
admin_kv admin_internal_validate_settings(const std::string & id, const json & settings, const admin_kv * base,
                                          std::vector<std::string> & warnings, json & locks);

// the saved entry of a model id: false when there is none; overrides = its settings as JSON
bool admin_internal_entry(const std::string & id, json & overrides, admin_kv & settings);

// false + why when the per-model settings cannot be saved right now (read-only file, state directory not writable)
bool admin_internal_settings_writable(std::string & why);

// Publish step: write the entry of an uploaded model before its files become visible (persist: false + error
// when the file cannot be written; nothing changes then). settings empty: an existing entry is removed.
// previous: the entry before (null = none), for admin_internal_restore_entry().
bool admin_internal_put_upload_entry(const std::string & id, const admin_kv & settings, json & previous, std::string & error);
bool admin_internal_restore_entry(const std::string & id, const json & previous, std::string & error);

//
// model control
//

// stops a running model and waits with the stage-2 deadline (stop-timeout + 10 s); throws a 504 stop_timeout
// admin_http_error when it does not stop. The caller holds admin_internal_apply_mutex().
void admin_internal_stop_model(const std::string & id);

// a deleted model: its saved settings (when remove_settings), its pin and the caches the admin layer keeps
// (facts of `paths`, fit checks, output tail, last failure). Returns what was removed.
json admin_internal_forget_model(const std::string & id, const std::vector<std::string> & paths, bool remove_settings);

// what the delete dry run reports
void admin_internal_model_flags(const std::string & id, bool & pinned, bool & has_settings);

//
// small helpers
//

std::string           admin_internal_getenv(const char * name); // UTF-8, "" when unset (any time, also before init)
std::filesystem::path admin_internal_path_from_utf8(const std::string & s);
std::string           admin_internal_path_to_utf8(const std::filesystem::path & p);
bool                  admin_internal_random_bytes(unsigned char * out, size_t n);
int64_t               admin_internal_now_ms();  // steady
int64_t               admin_internal_wall_ms(); // system clock
std::string           admin_internal_iso_time(int64_t wall_ms);
bool                  admin_internal_read_file(const std::filesystem::path & path, std::string & out, size_t max_size);
bool                  admin_internal_write_file(const std::filesystem::path & path, const std::string & content, bool private_mode);
bool                  admin_internal_dir_writable(const std::filesystem::path & dir);
void                  admin_internal_fsync_dir(const std::filesystem::path & dir); // renames in it durable (POSIX)
void                  admin_internal_dir_space(const std::filesystem::path & dir, uint64_t & total_bytes, uint64_t & free_bytes);
std::string           admin_internal_client_text(const server_http_req & req); // the client address as the allow-list sees it

//
// audit hook (stage 3, milestone M3 stores the records and shows them; until then they are only logged)
//
// One call per admin action with its outcome. Never blocks for long and never throws. req is the request that
// caused it (nullptr for an action of the server itself, e.g. an expired upload); details carries no secrets
// (no key, no token, no request body, no paths outside models_dir).
//
void admin_audit(const server_http_req * req, const std::string & action, const std::string & target,
                 const std::string & result, const json & details = json::object());
