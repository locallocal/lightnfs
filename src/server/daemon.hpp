#pragma once
// lightnfsd process lifecycle (design 01 §1.4): startup order config → identity →
// backends → engines → frontend, the SIGINT/SIGTERM wait loop with SIGHUP hot reload
// (plan doc 10 §4.1), and the mirror-image shutdown.  main.cpp only parses argv and
// dispatches here.

#include <string>

namespace lnfs::server {

// `--check-config`: parse + validate the TOML and construct the export table (so
// per-backend keys are checked exactly as a real startup would).  Exit code: 0 valid,
// 1 invalid (reason logged).
int check_config(const std::string& config_path);

// Runs the server until SIGINT/SIGTERM.  Exit code: 0 clean stop, 1 startup failure
// (reason logged).
int run_server(const std::string& config_path);

}  // namespace lnfs::server
