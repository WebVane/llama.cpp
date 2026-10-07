#include "server-context.h"
#include "server-http.h"
#include "server-models.h"
#include "server-cors-proxy.h"
#include "server-stream.h"
#include "server-tools.h"
#include "server-admin.h" // provision-admin

#include "arg.h"
#include "build-info.h"
#include "common.h"
#include "fit.h"
#include "llama.h"
#include "log.h"

#ifdef PROVISION_LICENSE
#include "license/LicenseLib.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#endif

#include <atomic>
#include <clocale>
#include <exception>
#include <signal.h>
#include <thread> // for std::thread::hardware_concurrency

#if defined(_WIN32)
#include <windows.h>
#endif

static std::function<void(int)> shutdown_handler;
static std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

static inline void signal_handler(int signal) {
    if (is_terminating.test_and_set()) {
        // in case it hangs, we can force terminate the server by hitting Ctrl+C twice
        // this is for better developer experience, we can remove when the server is stable enough
        fprintf(stderr, "Received second interrupt, terminating immediately.\n");
        exit(1);
    }

    shutdown_handler(signal);
}

// satisfies -Wmissing-declarations (used by llama command)
int llama_server(int argc, char ** argv);

// to be used via CLI (argc / argv are used by router mode only)
int llama_server(common_params & params, int argc, char ** argv);
void llama_server_terminate();
void llama_server_terminate() {
    if (shutdown_handler) {
        shutdown_handler(0);
    }
}


// wrapper function that handles exceptions and logs errors
// this is to make sure handler_t never throws exceptions; instead, it returns an error response
static server_http_context::handler_t ex_wrapper(server_http_context::handler_t func) {
    return [func = std::move(func)](const server_http_req & req) -> server_http_res_ptr {
        std::string message;
        error_type error;
        try {
            return func(req);
        } catch (const std::invalid_argument & e) {
            // treat invalid_argument as invalid request (400)
            error = ERROR_TYPE_INVALID_REQUEST;
            message = e.what();
        } catch (const std::exception & e) {
            // treat other exceptions as server error (500)
            error = ERROR_TYPE_SERVER;
            message = e.what();
        } catch (...) {
            error = ERROR_TYPE_SERVER;
            message = "unknown error";
        }

        auto res = std::make_unique<server_http_res>();
        res->status = 500;
        try {
            json error_data = format_error_response(message, error);
            res->status = json_value(error_data, "code", 500);
            res->data = safe_json_to_str({{ "error", error_data }});
            SRV_WRN("got exception: %s\n", res->data.c_str());
        } catch (const std::exception & e) {
            SRV_ERR("got another exception: %s | while handling exception: %s\n", e.what(), message.c_str());
            res->data = "Internal Server Error";
        }
        return res;
    };
}

#ifdef PROVISION_LICENSE
//
// License subsystem (src/license, shared with provision_ocr_server): signed
// .lic files, online activation against the License Server and an HMAC
// request counter. Modes: trial (N requests per install, then locked),
// online / offline (.lic installed), none (locked until activation).
//
// In router mode the parent process proxies every user request to a child
// instance; both run this same binary and share one state directory. The
// license is initialised and enforced only in the router / standalone
// process so each request is charged exactly once — child instances
// (spawned with LLAMA_SERVER_ROUTER_PORT set) pass through unmodified.
//

static bool license_is_child_server() {
    server_child probe;
    return probe.is_child();
}

// "trial" | "online" | "offline" | "none" — the vocabulary /license/status
// documents; the internal Token mode has no public equivalent yet.
static const char * license_public_mode(visio::license::Mode m) {
    using visio::license::Mode;
    switch (m) {
        case Mode::Trial:   return "trial";
        case Mode::Online:  return "online";
        case Mode::Offline: return "offline";
        default:            return "none";
    }
}

static std::string license_format_time(std::chrono::system_clock::time_point tp) {
    if (tp == std::chrono::system_clock::time_point{}) {
        return "";
    }
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Sanitised snapshot shared by GET /license/status and the activation
// response: mode / validity / budget only — no fingerprint, no signing key,
// nothing that identifies the machine.
static json license_status_json() {
    using namespace visio::license;
    auto & lib = LicenseLib::instance();
    const Decision d = lib.check_license();
    const auto cur = lib.current();

    const char * status_str = d.allow ? "valid" : (d.reason == DenyReason::Expired ? "expired" : "none");
    // -1 = a valid licence without a request quota (the server omits requestLimit)
    int64_t limit_out = d.limit;
    if (cur && !cur->limits.request_limit.has_value()) {
        limit_out = -1;
    }
    json body {
        {"mode",   license_public_mode(d.mode)},
        {"status", status_str},
        {"used",   d.used},
        {"limit",  limit_out},
        {"reason", d.allow ? "" : deny_reason_label(d.reason)},
    };
    if (cur) {
        body["product"]   = cur->product;
        body["tariff"]    = cur->tariff;
        body["expiresAt"] = license_format_time(cur->expires_at);
    }
    // licensed modes count in 30-day windows; the trial budget never resets
    if (const auto cs = lib.counter_snapshot();
        cs && d.mode != Mode::Trial && cs->limit > 0 && cs->window_start_unix > 0) {
        body["windowResetsAt"] = license_format_time(std::chrono::system_clock::from_time_t(
            (std::time_t) (cs->window_start_unix + kCounterWindowSeconds)));
    }
    return body;
}

// 403 for a refused request: the OpenAI-style error object clients and the
// web UI render, plus a machine-readable `license` block for dashboards.
static server_http_res_ptr license_denied_response(const visio::license::Decision & d, int64_t units = 1) {
    using namespace visio::license;
    std::string msg;
    switch (d.reason) {
        case DenyReason::RequestLimitReached:
            if (units > 1 && d.used < d.limit) {
                msg = string_format(
                    "This request would produce %lld results but only %lld of the %lld %s requests remain. "
                    "Send a smaller batch or activate a license (Settings -> License in the web UI, or POST /license/activate).",
                    (long long) units, (long long) (d.limit - d.used), (long long) d.limit,
                    d.mode == Mode::Trial ? "trial" : "licensed");
            } else if (d.mode == Mode::Trial) {
                msg = string_format(
                    "Trial limit reached: this evaluation build allows %lld inference requests and all of them are used. "
                    "Activate a license to continue (Settings -> License in the web UI, or POST /license/activate).",
                    (long long) d.limit);
            } else {
                msg = string_format("License request limit reached (%lld of %lld used in the current window).",
                    (long long) d.used, (long long) d.limit);
            }
            break;
        case DenyReason::NoLicense:
        case DenyReason::NotInitialised:
            msg = "No valid license: activate the product (Settings -> License in the web UI, or POST /license/activate).";
            break;
        case DenyReason::Expired:
            msg = "License expired: renew the license and activate again.";
            break;
        default:
            msg = std::string("License check refused the request: ") + deny_reason_label(d.reason);
            break;
    }
    auto res = std::make_unique<server_http_res>();
    json error_data = format_error_response(msg, ERROR_TYPE_PERMISSION);
    error_data["license"] = json {
        {"mode",   license_public_mode(d.mode)},
        {"reason", deny_reason_label(d.reason)},
        {"used",   d.used},
        {"limit",  d.limit},
    };
    res->status = json_value(error_data, "code", 403);
    res->data   = safe_json_to_str({{ "error", error_data }});
    res->headers["X-License-Mode"]  = license_public_mode(d.mode);
    res->headers["X-License-Used"]  = std::to_string(d.used);
    res->headers["X-License-Limit"] = std::to_string(d.limit);
    if (d.mode == Mode::Trial) {
        // kept for tooling that grew up with the trial-only build
        res->headers["X-Trial-Limit"] = std::to_string(d.limit);
        res->headers["X-Trial-Used"]  = std::to_string(d.used);
    }
    return res;
}

// Number of results one request produces — the unit of metering, so a trial
// cannot be stretched by batching. Which field is the batch depends on the
// endpoint: /completion(s) `prompt` arrays and n / n_cmpl completions,
// /embedding(s) `input` (legacy: `content`) arrays, /rerank `documents`.
// Arrays of numbers are one tokenized prompt, not a batch. Everything else
// (responses, whose `input` is a message list, messages, infill, audio
// uploads) is one unit.
static int64_t license_request_units(const server_http_req & req) {
    const char * batch_key     = nullptr;
    const char * batch_key_alt = nullptr;
    bool         has_n         = false;
    if (string_ends_with(req.path, "/completion") || string_ends_with(req.path, "/completions")) {
        batch_key = "prompt";
        has_n     = true;
    } else if (string_ends_with(req.path, "/embedding") || string_ends_with(req.path, "/embeddings")) {
        batch_key     = "input";
        batch_key_alt = "content";
    } else if (string_ends_with(req.path, "/rerank") || string_ends_with(req.path, "/reranking")) {
        batch_key = "documents";
    } else {
        return 1;
    }
    int64_t units = 1;
    try {
        const json body = json::parse(req.body);
        if (!body.is_object()) {
            return 1;
        }
        for (const char * key : {batch_key, batch_key_alt}) {
            if (!key || !body.contains(key)) {
                continue;
            }
            const json & arr = body.at(key);
            if (arr.is_array() && arr.size() > 0 && !json_is_array_and_contains_numbers(arr)) {
                units = std::max(units, (int64_t) arr.size());
            }
        }
        if (has_n) {
            const int64_t n = json_value(body, "n", json_value(body, "n_cmpl", (int64_t) 1));
            units *= std::max((int64_t) 1, n);
        }
    } catch (const std::exception &) {
        // not JSON: one unit
    }
    return std::max((int64_t) 1, units);
}

// wrapper that gates a handler behind the license, reserve-then-commit: the
// request's units are reserved atomically BEFORE inference (so concurrent
// requests cannot all pass a read-only check for the same last slot and run
// work nobody is charged for) and refunded when the result is not delivered
// — a 400 / 404 / 503 from the handler (bad request, model failed to load,
// queue full), a client that hung up while waiting (the handlers return an
// empty 200 then), or a stream torn down before its end. Only inference
// endpoints are wrapped (see route registration below) — health, metrics,
// props, models, tokenize and the web UI assets stay free.
static server_http_context::handler_t license_gate(server_http_context::handler_t func) {
    if (license_is_child_server()) {
        return func;
    }
    return [func = std::move(func)](const server_http_req & req) -> server_http_res_ptr {
        auto & lib = visio::license::LicenseLib::instance();
        const int64_t units = license_request_units(req);
        if (const auto d = lib.authorize_request(units); !d.allow) {
            return license_denied_response(d, units);
        }
        SRV_DBG("license: reserved %lld unit(s) for %s\n", (long long) units, req.path.c_str());
        server_http_res_ptr res;
        try {
            res = func(req);
        } catch (...) {
            SRV_DBG("license: refund %lld unit(s), handler threw (%s)\n", (long long) units, req.path.c_str());
            lib.refund_request(units);
            throw;
        }
        if (!res || res->status < 200 || res->status >= 300 || req.should_stop()) {
            SRV_DBG("license: refund %lld unit(s), not delivered (status=%d, client_gone=%d)\n",
                    (long long) units, res ? res->status : 0, (int) req.should_stop());
            lib.refund_request(units);
            return res;
        }
        if (res->is_stream()) {
            // a stream counts once it ran to its end; if the response dies
            // before that (client gone, write error) the reservation is
            // returned by the guard's destructor
            struct stream_charge {
                int64_t units = 0;
                bool    done  = false;
                ~stream_charge() {
                    if (!done) {
                        SRV_DBG("license: refund %lld unit(s), stream did not finish\n", (long long) units);
                        visio::license::LicenseLib::instance().refund_request(units);
                    }
                }
            };
            // constructed in place: a temporary copied into the shared_ptr would
            // run this destructor immediately and refund the reservation
            auto charge = std::make_shared<stream_charge>();
            charge->units = units;
            auto orig_next = std::move(res->next);
            std::function<bool()> should_stop = req.should_stop;
            res->next = [orig_next = std::move(orig_next), charge, should_stop](std::string & out) {
                const bool more = orig_next(out);
                if (!more) {
                    const bool gone = should_stop();
                    SRV_DBG("license: stream ended, client_gone=%d\n", (int) gone);
                    if (!gone) {
                        charge->done = true;
                    }
                }
                return more;
            };
        }
        return res;
    };
}

// Startup: resolve the license mode (persisted .lic -> env activation code ->
// trial fallback). Never fatal: with nothing usable the server starts LOCKED
// (inference -> 403) so /health, /license/status and /license/activate stay
// reachable and the operator can activate without a restart.
static void license_startup() {
    using namespace visio::license;
    auto & lib = LicenseLib::instance();
    const auto cfg = Config::from_defaults();
    auto r = lib.init(cfg);
    const bool have_valid_lic = r.ok && (r.mode == Mode::Online || r.mode == Mode::Offline);

    if (have_valid_lic) {
        LOG_INF("[license] using existing .lic at %s; mode=%s detail=%s\n",
                cfg.lic_path.string().c_str(), mode_label(r.mode), r.detail.c_str());
    } else if (const char * code = std::getenv("PROVISION_LICENSE_ACTIVATION_CODE"); code && *code) {
        LOG_INF("%s", "[license] no valid .lic - running online activation (PROVISION_LICENSE_ACTIVATION_CODE is set)\n");
        const auto a = lib.activate(code);
        if (a.ok) {
            // activate() ends with a re-init and reports its outcome
            LOG_INF("[license] activated; mode=%s detail=%s\n", mode_label(a.mode), a.detail.c_str());
        } else if (r.ok) {
            LOG_WRN("[license] activation failed (%s); falling back to mode=%s\n", a.detail.c_str(), mode_label(r.mode));
        } else {
            LOG_ERR("[license] activation failed and no fallback available: %s\n", a.detail.c_str());
            LOG_ERR("[license] starting LOCKED (mode=none): inference requests get 403 until activation succeeds or a .lic is placed at %s\n",
                    cfg.lic_path.string().c_str());
        }
    } else if (!r.ok) {
        LOG_WRN("[license] no license sources - starting LOCKED (mode=none): %s\n", r.detail.c_str());
        LOG_WRN("[license] activate via POST /license/activate, set PROVISION_LICENSE_ACTIVATION_CODE, or place a .lic at %s\n",
                cfg.lic_path.string().c_str());
    } else {
        // trial resolved normally
        const auto d = lib.check_license();
        LOG_WRN("[license] mode=%s: %s (%lld of %lld requests used)\n",
                mode_label(r.mode), r.detail.c_str(), (long long) d.used, (long long) d.limit);
    }
    LOG_INF("[license] product=%s activation_url=%s state_dir=%s\n",
            cfg.product.c_str(),
            cfg.activation_url.empty() ? "<not configured>" : cfg.activation_url.c_str(),
            cfg.state_dir.string().c_str());
}

static server_http_res_ptr license_status_handler(const server_http_req &) {
    auto res = std::make_unique<server_http_res>();
    res->status = 200;
    res->data   = safe_json_to_str(license_status_json());
    return res;
}

static server_http_res_ptr license_activate_handler(const server_http_req & req) {
    auto res = std::make_unique<server_http_res>();

    std::string code;
    try {
        const json body = json::parse(req.body);
        code = json_value(body, "activationCode", std::string());
    } catch (const std::exception &) {
        // reported as a bad request below
    }
    // activation codes are short printable ASCII (XXXX-XXXX-XXXX); anything
    // else is refused before it can reach the License Server
    const bool printable_ascii = std::all_of(code.begin(), code.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
    if (code.empty() || code.size() > 256 || !printable_ascii) {
        json err = format_error_response(
            "activationCode must be a printable ASCII string of at most 256 characters", ERROR_TYPE_INVALID_REQUEST);
        res->status = json_value(err, "code", 400);
        res->data   = safe_json_to_str({{ "error", err }});
        return res;
    }
    // concurrent activations are serialised inside LicenseLib: a second caller
    // waits for the first round-trip and gets the idempotent result (the
    // License Server answers the same machine + code with the same .lic)
    const auto r = visio::license::LicenseLib::instance().activate(code);

    if (r.ok) {
        LOG_INF("[license] /license/activate: activated; mode=%s detail=%s\n",
                visio::license::mode_label(r.mode), r.detail.c_str());
    } else {
        LOG_WRN("[license] /license/activate failed: %s\n", r.detail.c_str());
    }
    json body = license_status_json();
    body["activated"] = r.ok;
    body["detail"]    = r.detail;
    if (!r.ok) {
        body["message"] = r.detail; // what API clients / the web UI show for the 502
    }
    // 502: activation is a proxied operation against the License Server —
    // bad code, no free slot and LS unreachable all surface here with the reason
    res->status = r.ok ? 200 : 502;
    res->data   = safe_json_to_str(body);
    return res;
}
#else
// no-op pass-through when the license subsystem is not compiled in
static inline server_http_context::handler_t license_gate(server_http_context::handler_t func) {
    return func;
}
#endif

int llama_server(int argc, char ** argv) {
    if (int rc = 0; server_admin_helper_main(argc, argv, rc)) { return rc; } // provision-admin: helper modes (device inventory, fit check)
    std::setlocale(LC_NUMERIC, "C");

#ifndef _WIN32
    // Ignore SIGPIPE so the server does not crash if a child (MCP server, tools runtime) exits while we are writing to its stdin
    signal(SIGPIPE, SIG_IGN);
#endif

    // own arguments required by this example
    common_params params;

    common_init();

    // start the stream session manager GC right after common init, before any HTTP route can
    // touch it. lifecycle is symmetric, stop_gc() runs in clean_up() before backend free
    server_stream_session_manager_start();

    SRV_INF("%s", "initializing ...\n");

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }

    llama_backend_init();
    llama_numa_init(params.numa);

    const int result = llama_server(params, argc, argv);
    common_log_flush(common_log_main());
    return result;
}

int llama_server(common_params & params, int argc, char ** argv) {
    bool is_run_by_cli = (argv == nullptr);

    common_models_handler models_handler;

    // note: router mode also accepts -hf remote-preset, so we need to check that first
    if (!is_run_by_cli && !params.model.hf_repo.empty()) {
        try {
            models_handler = common_models_handler_init(params, LLAMA_EXAMPLE_SERVER);
            if (common_models_handler_is_preset_repo(models_handler)) {
                // apply the preset and start the server in router mode
                common_models_handler_apply(models_handler, params);
            }
        } catch (const std::exception & e) {
            SRV_ERR("failed to fetch model metadata: %s\n", e.what());
            return 1;
        }
    }

    // router server never loads a model and must not touch the GPU
    const bool is_router_server = params.model.path.empty()
                               && params.model.hf_repo.empty()
                               && params.model.docker_repo.empty();

    // skip device enumeration so the CUDA primary context stays uncreated
    common_params_print_info(params, !is_router_server);

    if (!is_router_server) {
        // validate batch size for embeddings
        // embeddings require all tokens to be processed in a single ubatch
        // see https://github.com/ggml-org/llama.cpp/issues/12836
        if (params.embedding && params.n_batch > params.n_ubatch) {
            SRV_WRN("embeddings enabled with n_batch (%d) > n_ubatch (%d)\n", params.n_batch, params.n_ubatch);
            SRV_WRN("setting n_batch = n_ubatch = %d to avoid assertion failure\n", params.n_ubatch);
            params.n_batch = params.n_ubatch;
        }

        if (params.n_parallel < 0) {
            SRV_TRC("%s", "n_parallel is set to auto, using n_parallel = 4 and kv_unified = true\n");

            params.n_parallel = 4;
            params.kv_unified = true;
        }
    }

    // size the KV pool from --kv-unified-per-slot, unless the user pinned it with -c
    // or with -c 0 for max context
    const bool ctx_pool_auto_sized = params.kv_unified_per_slot > 0 &&
                                     params.n_ctx == 0 &&
                                     (uint32_t) params.fit_params_min_ctx != UINT32_MAX;

    if (ctx_pool_auto_sized) {
        params.n_ctx = params.n_parallel * params.kv_unified_per_slot;
        SRV_INF("--kv-unified-per-slot: sizing KV pool to n_parallel * kv_unified_per_slot = %d * %d = %d\n", params.n_parallel,
                params.kv_unified_per_slot, params.n_ctx);
    }

    // for consistency between server router mode and single-model mode, we set the same model name as alias
    auto model_name = params.model.get_name();
    if (params.model_alias.empty() && !model_name.empty()) {
        params.model_alias.insert(model_name);
    }

    // note: this is guaranteed to out-live ctx_http and tools
    server_mcp mcp_mgr;

    // struct that contains llama context and inference
    server_context ctx_server;

    //
    // Router
    //

    // register API routes
    server_child child; // only used in non-router mode
    server_routes routes(params, ctx_server);
    server_tools tools;

    std::optional<server_models_routes> models_routes{};

#ifdef PROVISION_LICENSE
    if (!license_is_child_server()) {
        license_startup();
    }
#endif

    server_http_context ctx_http;
    if (!ctx_http.init(params)) {
        SRV_ERR("%s", "failed to initialize HTTP server\n");
        return 1;
    }

    if (is_router_server) {
        // provision-admin: read and scrub PROVISION_ADMIN*, load the saved settings (may override
        // params.models_max); must run before server_models snapshots the environment for its children
        server_admin_init(params);

        // setup server instances manager
        try {
            models_routes.emplace(params, argc, argv);
        } catch (const std::exception & e) {
            SRV_ERR("failed to initialize router models: %s\n", e.what());
            return 1;
        }

        // proxy handlers
        // note: routes.get_health stays the same
        routes.get_metrics                 = models_routes->proxy_get;
        routes.post_props                  = models_routes->proxy_post;
        routes.post_completions            = models_routes->proxy_post;
        routes.post_completions_oai        = models_routes->proxy_post;
        routes.post_chat_completions       = models_routes->proxy_post;
        routes.post_control                = models_routes->proxy_post;
        routes.post_responses_oai          = models_routes->proxy_post;
        routes.post_transcriptions_oai     = models_routes->proxy_post;
        routes.post_anthropic_messages     = models_routes->proxy_post;
        routes.post_anthropic_count_tokens = models_routes->proxy_post;
        routes.post_infill                 = models_routes->proxy_post;
        routes.post_embeddings             = models_routes->proxy_post;
        routes.post_embeddings_oai         = models_routes->proxy_post;
        routes.post_rerank                 = models_routes->proxy_post;
        routes.post_tokenize               = models_routes->proxy_post;
        routes.post_detokenize             = models_routes->proxy_post;
        routes.post_apply_template         = models_routes->proxy_post;
        routes.post_chat_completions_tok   = models_routes->proxy_post;
        routes.post_responses_tok_oai      = models_routes->proxy_post;
        routes.get_lora_adapters           = models_routes->proxy_get;
        routes.post_lora_adapters          = models_routes->proxy_post;
        routes.get_slots                   = models_routes->proxy_get;
        routes.post_slots                  = models_routes->proxy_post;

        // custom routes for router
        routes.get_props                   = models_routes->get_router_props;
        routes.get_models                  = models_routes->get_router_models;

        ctx_http.post("/models",               ex_wrapper(models_routes->post_router_models));
        ctx_http.post("/models/load",          ex_wrapper(models_routes->post_router_models_load));
        ctx_http.post("/models/unload",        ex_wrapper(models_routes->post_router_models_unload));
        ctx_http.get ("/models/sse",           ex_wrapper(models_routes->get_router_models_sse));
        ctx_http.del ("/models",               ex_wrapper(models_routes->del_router_models));

        // provision-admin: the /admin/* routes, registered only when PROVISION_ADMIN is enabled
        server_admin_register_routes(ctx_http, *models_routes);
    }

    ctx_http.get ("/health",                   ex_wrapper(routes.get_health)); // public endpoint (no API key check)
    ctx_http.get ("/v1/health",                ex_wrapper(routes.get_health)); // public endpoint (no API key check)
    ctx_http.get ("/metrics",                  ex_wrapper(routes.get_metrics));
    ctx_http.get ("/props",                    ex_wrapper(routes.get_props));
    ctx_http.post("/props",                    ex_wrapper(routes.post_props));
    ctx_http.get ("/models",                   ex_wrapper(routes.get_models));
    ctx_http.get ("/v1/models",                ex_wrapper(routes.get_models));
    // inference endpoints are gated by the license subsystem (no-op when it is not compiled in)
    ctx_http.post("/completion",               license_gate(ex_wrapper(routes.post_completions))); // legacy
    ctx_http.post("/completions",              license_gate(ex_wrapper(routes.post_completions)));
    ctx_http.post("/v1/completions",           license_gate(ex_wrapper(routes.post_completions_oai)));
    ctx_http.post("/chat/completions",         license_gate(ex_wrapper(routes.post_chat_completions)));
    ctx_http.post("/v1/chat/completions",      license_gate(ex_wrapper(routes.post_chat_completions)));
    ctx_http.post("/v1/chat/completions/control", ex_wrapper(routes.post_control)); // control channel (not gated)
    ctx_http.post("/v1/responses",             license_gate(ex_wrapper(routes.post_responses_oai)));
    ctx_http.post("/responses",                license_gate(ex_wrapper(routes.post_responses_oai)));
    ctx_http.post("/v1/audio/transcriptions",  license_gate(ex_wrapper(routes.post_transcriptions_oai)));
    ctx_http.post("/audio/transcriptions",     license_gate(ex_wrapper(routes.post_transcriptions_oai)));
    ctx_http.post("/v1/messages",              license_gate(ex_wrapper(routes.post_anthropic_messages))); // anthropic messages API
    ctx_http.post("/infill",                   license_gate(ex_wrapper(routes.post_infill)));
    ctx_http.post("/embedding",                license_gate(ex_wrapper(routes.post_embeddings))); // legacy
    ctx_http.post("/embeddings",               license_gate(ex_wrapper(routes.post_embeddings)));
    ctx_http.post("/v1/embeddings",            license_gate(ex_wrapper(routes.post_embeddings_oai)));
    ctx_http.post("/rerank",                   license_gate(ex_wrapper(routes.post_rerank)));
    ctx_http.post("/reranking",                license_gate(ex_wrapper(routes.post_rerank)));
    ctx_http.post("/v1/rerank",                license_gate(ex_wrapper(routes.post_rerank)));
    ctx_http.post("/v1/reranking",             license_gate(ex_wrapper(routes.post_rerank)));
    ctx_http.post("/tokenize",                 ex_wrapper(routes.post_tokenize));
    ctx_http.post("/detokenize",               ex_wrapper(routes.post_detokenize));
    ctx_http.post("/apply-template",           ex_wrapper(routes.post_apply_template));
#ifdef PROVISION_LICENSE
    // license status is public (read-only, never metered); activation goes
    // through the regular API-key check. Both live only in the process that
    // enforces the license (router / standalone), not in child instances.
    if (!license_is_child_server()) {
        ctx_http.get ("/license/status",   ex_wrapper(license_status_handler));
        ctx_http.post("/license/activate", ex_wrapper(license_activate_handler));
    }
#endif
    // token counting
    ctx_http.post("/chat/completions/input_tokens",    ex_wrapper(routes.post_chat_completions_tok));
    ctx_http.post("/v1/chat/completions/input_tokens", ex_wrapper(routes.post_chat_completions_tok));
    ctx_http.post("/responses/input_tokens",           ex_wrapper(routes.post_responses_tok_oai));
    ctx_http.post("/v1/responses/input_tokens",        ex_wrapper(routes.post_responses_tok_oai));
    ctx_http.post("/v1/messages/count_tokens",         ex_wrapper(routes.post_anthropic_count_tokens)); // anthropic token counting
    // LoRA adapters hotswap
    ctx_http.get ("/lora-adapters",            ex_wrapper(routes.get_lora_adapters));
    ctx_http.post("/lora-adapters",            ex_wrapper(routes.post_lora_adapters));
    // Save & load slots
    ctx_http.get ("/slots",                    ex_wrapper(routes.get_slots));
    ctx_http.post("/slots/:id_slot",           ex_wrapper(routes.post_slots));

    // resumable streaming: a child binds the local session factories, the router binds
    // proxies that resolve the owning child, see server-stream.h
    server_http_context::handler_t stream_get_h;
    server_http_context::handler_t streams_lookup_h;
    server_http_context::handler_t stream_delete_h;
    if (is_router_server) {
        stream_get_h     = models_routes->router_stream_get;
        streams_lookup_h = models_routes->router_streams_lookup;
        stream_delete_h  = models_routes->router_stream_delete;
    } else {
        stream_get_h     = server_stream_make_get_handler();
        streams_lookup_h = server_stream_make_lookup_handler();
        stream_delete_h  = server_stream_make_delete_handler();
    }
    ctx_http.get ("/v1/stream",                ex_wrapper(stream_get_h));
    ctx_http.post("/v1/streams/lookup",        ex_wrapper(streams_lookup_h));
    ctx_http.del ("/v1/stream",                ex_wrapper(stream_delete_h));

    // Google Cloud Platform (Vertex AI) compat
    ctx_http.register_gcp_compat();

    // return 403 for disabled features
    server_http_context::handler_t res_403 = [](const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        res->status = 403;
        res->data = safe_json_to_str({
            {"error", {
                {"message", "this feature is disabled"},
                {"type", "feature_disabled"},
            }}
        });
        return res;
    };

    if (params.cors_origins == "*" && params.api_keys.empty()) {
        SRV_WRN("%s", "security: no API key is set and CORS allows all origins (see https://github.com/ggml-org/llama.cpp/pull/25655)\n");
    }

    // CORS proxy (EXPERIMENTAL, only used by the Web UI for MCP)
    std::vector<std::string> warn_names;
    if (is_router_server) {
        warn_names.push_back("router mode");
    }

    if (params.ui_mcp_proxy) {
        ctx_http.get ("/cors-proxy",      ex_wrapper(proxy_handler_get));
        ctx_http.post("/cors-proxy",      ex_wrapper(proxy_handler_post));
        warn_names.push_back("MCP proxy (experimental)");
    } else {
        ctx_http.get ("/cors-proxy",      ex_wrapper(res_403));
        ctx_http.post("/cors-proxy",      ex_wrapper(res_403));
    }

    try {
        mcp_mgr.start(params);
    } catch (const std::exception & e) {
        SRV_ERR("MCP starting failed: %s\n", e.what());
        return 1;
    }

    if (!params.server_tools.empty() || !mcp_mgr.empty()) {
        try {
            tools.setup(params.server_tools, mcp_mgr, params.server_tools_runtime);
        } catch (const std::exception & e) {
            SRV_ERR("tools setup failed: %s\n", e.what());
            return 1;
        }
        ctx_http.get ("/tools",           ex_wrapper(tools.handle_get));
        ctx_http.post("/tools",           ex_wrapper(tools.handle_post));
        if (!params.server_tools.empty()) {
            warn_names.push_back("server tools (experimental)");
        }
        if (!params.server_tools_runtime.empty()) {
            warn_names.push_back("tools runtime (experimental)");
        }
        if (!mcp_mgr.empty()) {
            warn_names.push_back("MCP servers (experimental)");
        }
    } else {
        ctx_http.get ("/tools",           ex_wrapper(res_403));
        ctx_http.post("/tools",           ex_wrapper(res_403));
    }

    if (!warn_names.empty()) {
        std::string features;
        for (const auto & name : warn_names) {
            if (!features.empty()) features += ", ";
            features += name;
        }
        SRV_WRN("security: %s enabled - do not expose to untrusted environments\n", features.c_str());
    }

    //
    // Handle downloading model
    //

    if (child.is_child() && child.get_mode() == SERVER_CHILD_MODE_DOWNLOAD) {
        return child.run_download(params);
    } else if (!is_router_server && !is_run_by_cli) {
        // single-model mode (NOT spawned by router)
        // if this is invoked by CLI, model downloading should be already handled
        try {
            common_models_handler_apply(models_handler, params);
        } catch (const std::exception & e) {
            SRV_ERR("failed to download model: %s\n", e.what());
            return 1;
        }
    }

    //
    // Start the server
    //

    std::function<void()> clean_up;

    if (is_router_server) {
        SRV_INF("%s", "starting server in router mode. models will be automatically loaded on-demand\n");

        clean_up = [&models_routes, &mcp_mgr]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            // stop the session GC first, it finalizes live sessions and wakes pending readers
            server_stream_session_manager_stop();
            if (models_routes.has_value()) {
                models_routes->stopping.store(true); // maybe redundant, but just to be safe
                models_routes->models.unload_all();
            }
            mcp_mgr.shutdown();
            llama_backend_free();
        };

        if (!ctx_http.start()) {
            clean_up();
            SRV_ERR("%s", "exiting due to HTTP server error\n");
            return 1;
        }
        ctx_http.is_ready.store(true);

        shutdown_handler = [&](int) {
            if (models_routes.has_value()) {
                // important to disconnect any SSE clients
                models_routes->stopping.store(true);
            }
            mcp_mgr.shutdown();
            ctx_http.stop();
        };

        try {
            models_routes->models.load_startup_models();
        } catch (const std::exception & e) {
            SRV_ERR("failed to load models on startup: %s\n", e.what());
            ctx_http.stop();
            ctx_http.join();
            clean_up();
            return 1;
        }

    } else {
        // setup clean up function, to be called before exit
        clean_up = [&ctx_http, &ctx_server, &mcp_mgr]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            // stop the session GC first, it finalizes live sessions and wakes pending readers
            server_stream_session_manager_stop();
            ctx_http.stop();
            ctx_server.terminate();
            mcp_mgr.shutdown();
            llama_backend_free();
        };

        // start the HTTP server before loading the model to be able to serve /health requests
        if (!ctx_http.start()) {
            clean_up();
            SRV_ERR("%s", "exiting due to HTTP server error\n");
            return 1;
        }

        // setup communication child --> router if necessary
        if (child.is_child()) {
            ctx_server.set_state_callback([&](server_state state, json payload) {
                child.notify_to_router(server_state_to_str(state), payload);
            });
        }

        if (!ctx_server.load_model(params)) {
            clean_up();
            ctx_http.join();
            SRV_ERR("%s", "exiting due to model loading error\n");
            return 1;
        }

        routes.update_meta(ctx_server);
        ctx_http.is_ready.store(true);

        SRV_INF("%s", "model loaded\n");

        shutdown_handler = [&](int) {
            mcp_mgr.shutdown();
            // this will unblock start_loop()
            ctx_server.terminate();
        };
    }

    // register signal handler if not running by CLI
    if (!is_run_by_cli) {
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
        struct sigaction sigint_action;
        sigint_action.sa_handler = signal_handler;
        sigemptyset (&sigint_action.sa_mask);
        sigint_action.sa_flags = 0;
        sigaction(SIGINT, &sigint_action, NULL);
        sigaction(SIGTERM, &sigint_action, NULL);
#elif defined (_WIN32)
        auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
            return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
        };
        SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif
    }

    bool uses_default_port = false;
    for (const auto & address : ctx_http.listening_addresses) {
        SRV_INF("listening on %s\n", address.c_str());
        uses_default_port |= string_ends_with(address, ":8080");
    }

    // TODO: remove this in the future
    // check the string to also handle the .sock case
    if (uses_default_port) {
        SRV_WRN("%s", "notice: server default port will be changed to :9931 in a future release (ref: https://github.com/ggml-org/llama.cpp/pull/26508)\n");
    }

    if (is_router_server) {
        if (!params.models_preset_hf.empty()) {
            SRV_WRN(      "NOTE: using preset.ini from HF repo '%s'\n", params.models_preset_hf.c_str());
            SRV_WRN("%s", "      please only use presets that you can trust! Unknown presets may be unsafe\n");
        }

        ctx_http.join(); // keep the main thread alive

        // when the HTTP server stops, clean up and exit
        clean_up();
    } else {
        // optionally, notify router server that this instance is ready
        std::thread monitor_thread;
        if (child.is_child()) {
            monitor_thread = child.setup(shutdown_handler);
            child.notify_to_router(server_state_to_str(SERVER_STATE_READY), routes.get_model_info());
        }

        // this call blocks the main thread until queue_tasks.terminate() is called
        ctx_server.start_loop();

        clean_up();
        ctx_http.join();
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }

        auto * ll_ctx = ctx_server.get_llama_context();
        if (ll_ctx != nullptr) {
            common_memory_breakdown_print(ll_ctx);
        }
    }

    return 0;
}
