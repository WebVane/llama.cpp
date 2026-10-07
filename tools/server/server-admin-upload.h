#pragma once

//
// Provision admin layer, stage 3 (fork-owned, see docs/provision-admin.md): GGUF upload into
// --models-dir, delete of models_dir models and "Remove projector".
//
// Routes (all under /admin, admin key required, registered by server_admin_register_routes()):
//   GET  /admin/models/upload            availability, limits, open sessions
//   GET  /admin/models/upload?id=        one session (resume)
//   POST /admin/models/upload            create a session (or dry_run)
//   POST /admin/models/upload/chunk      append one chunk (raw application/octet-stream body)
//   POST /admin/models/upload/update     change name / settings / auto_publish / expected SHA-256, drop a file
//   POST /admin/models/upload/publish    publish a ready session (or retry its checks)
//   POST /admin/models/upload/cancel     discard a session
//   POST /admin/models/delete            delete a models_dir model (or only its projector), dry_run first
//
// Uploads are staged in <models_dir>/.provision-upload/<id>/ (never listed by the models_dir scanner), checked
// by a helper process of this binary (PROVISION_ADMIN_HELPER=validate) and published with one rename that never
// replaces anything. The authoritative session record lives in the admin state directory.
//

#include "server-common.h"
#include "server-http.h"

#include <string>
#include <vector>

struct common_params;

// reads the test-only PROVISION_ADMIN_TEST_* hooks of the upload code; called by server_admin_init()
// before it removes every PROVISION_ADMIN* variable from the environment
void admin_upload_read_env();

// after server_models exists (from server_admin_register_routes): models_dir, staging, recovery of the
// sessions of an earlier run, the background worker
void admin_upload_init(const common_params & params);

// registers the routes above and the pre-body check of the chunk route; appends every path it registered to
// `paths` (the caller removes them from the generic handler table, see server_admin_register_routes())
void admin_upload_register(server_http_context & ctx_http, std::vector<std::string> & paths);

// additive fields of GET /admin/status ("upload") and GET /admin/storage ("uploads")
json admin_upload_status_json();
json admin_upload_storage_json();

// called about once a second from the admin events thread: cheap, it only wakes the upload worker when the
// sweep is due (the worker does the deletions, never the events thread)
void admin_upload_tick();

// stops the worker and releases the session locks; before server_models is destroyed
void admin_upload_stop();

// PROVISION_ADMIN_HELPER=validate: checks GGUF files for an upload and prints one result line
int admin_upload_helper_validate(int argc, char ** argv);

// false (and why) when the metadata of a GGUF file is larger than the admin layer reads in the router, or the
// file is not a GGUF file at all; a cheap streaming scan that allocates nothing per value
bool admin_gguf_header_within_limit(const std::string & path, std::string & why);
