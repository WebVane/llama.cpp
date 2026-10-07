#pragma once

//
// Provision admin layer, stage 3 milestone M3 (fork-owned, see docs/provision-admin.md): the audit log of admin
// actions.
//
// Kinds of records:
//   change    a change made through an /admin route with the admin key (settings, per-model settings, GPU
//             re-detection, the upload lifecycle, delete, remove projector), with its outcome
//   refusal   401 / 403 answers of the /admin routes, merged per client, path and status for 60 s
//   server    actions of the server itself (settings reverted by the circuit breaker or at startup, expired and
//             swept uploads, audit.started)
//   observed  effects the public API caused, seen on the model event feed (download finished / failed, model
//             removed, model start failed): who caused them is unknown
//
// Storage: <state>/admin-audit.jsonl (changes, server), <state>/admin-audit-refusals.jsonl (refusals) and
// <state>/admin-audit-observed.jsonl (observed), JSON lines of at most 4 KiB, appended and fsynced per record,
// private mode, each rotated to .1 at 4 MiB. Refusals and observed effects can be caused without the admin key, so
// they live in files of their own with a global rate cap: a flood of them can never push a change record out. When
// a file cannot be written (at startup or later) its records go to an in-memory ring of 1000 instead (GET
// /admin/status warns audit_not_persisted), and writing is tried again every minute (the ring in one write and one
// fsync per 4 MiB); an admin action never fails because of the audit and waits at most for that write.
//
// Route (admin key required, registered by server_admin_register_routes()):
//   GET /admin/audit?before=&limit=&kind=&action=&result=[&format=jsonl]
//
// Never stored: the admin key, Authorization, HF_TOKEN, request bodies, paths outside models_dir.
//
// A line that a crash cut (no newline at the end of a file) stays on a line of its own: the next record starts on
// a new line.
//

#include "server-common.h"
#include "server-http.h"

#include <filesystem>
#include <string>
#include <vector>

// reads the test-only PROVISION_ADMIN_TEST_AUDIT_* hooks; called by server_admin_init() before it removes every
// PROVISION_ADMIN* variable from the environment
void admin_audit_read_env();

// opens the store (state_dir_ok: the state directory exists) and writes audit.started; called by
// server_admin_init() once the state directory is known, before anything else is recorded
void admin_audit_store_init(const std::filesystem::path & state_dir, bool state_dir_ok, const std::string & version);

// a value that must never appear in a record (the admin key, HF_TOKEN): replaced by "[redacted]"
void admin_audit_add_secret(const std::string & value);

// text / every string (values and keys) of a JSON value with those secrets replaced: for what is cut before it is
// recorded (header values, keys) and for the log line of a record
std::string admin_audit_redact_text(const std::string & text);
json admin_audit_redact_json(const json & value);

// one record of kind change | server | observed; actor from admin_audit_actor() (or a server / public API actor)
void admin_audit_store_add(const std::string & kind, const json & actor, const std::string & action,
                           const std::string & target, const std::string & result, const json & details);

// one refused request (401 / 403): merged per client + path + status for 60 s (probe: the no-key status probe,
// merged per path + status only, since every browser of a refused address calls it on every page load)
void admin_audit_store_refusal(const json & actor, const std::string & client, const std::string & path, int status,
                               const std::string & code, const std::string & reason, bool probe);

// about once a second from the admin events thread: writes merged refusals whose window ended, retries writing
// after an error; cheap
void admin_audit_tick();

// writes every open merge window (server shutdown)
void admin_audit_flush();

// registers GET /admin/audit; appends the path to `paths` (removed from the generic handler table by the caller)
void admin_audit_register(server_http_context & ctx_http, std::vector<std::string> & paths);

// additive fields: "audit" of GET /admin/status and of GET /admin/storage, the diagnostics item
json admin_audit_status_json();
json admin_audit_storage_json();
bool admin_audit_persisted();
void admin_audit_diagnostics(std::string & status, json & value, std::string & hint);
