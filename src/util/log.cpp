#include "util/log.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>  // also provides stderr_sink_mt

#include <memory>
#include <mutex>

namespace lnfs {
namespace detail {
namespace {

// Matches the pre-spdlog format: `ts=<epoch-seconds>.<ms> level=<name> <message>`.
// (spdlog spells the warn level "warning" — nothing greps the level column.)
constexpr char kPattern[] = "ts=%E.%e level=%l %v";

std::once_flag g_once;

void ensure_default_logger() {
  std::call_once(g_once, [] {
    auto lg = std::make_shared<spdlog::logger>(
        "lnfs", std::make_shared<spdlog::sinks::stderr_sink_mt>());
    lg->set_pattern(kPattern);
    lg->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(lg));
  });
}

}  // namespace

spdlog::logger* logger() {
  ensure_default_logger();
  return spdlog::default_logger_raw();
}

}  // namespace detail

void init_async_logging(const LogSinkConfig& cfg) {
  auto level = detail::logger()->level();  // keep what set_log_level configured
  spdlog::init_thread_pool(8192, 1);
  spdlog::sink_ptr sink;
  if (cfg.file.empty()) {
    sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
  } else {
    // Size-based rotation (plan doc 10 §4.4): file, file.1 .. file.<keep>.
    sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        cfg.file, cfg.rotate_size, cfg.rotate_keep);
  }
  auto lg = std::make_shared<spdlog::async_logger>(
      "lnfs", std::move(sink), spdlog::thread_pool(),
      spdlog::async_overflow_policy::discard_new);
  lg->set_pattern(detail::kPattern);
  lg->set_level(level);
  spdlog::set_default_logger(std::move(lg));
}

void shutdown_async_logging() { spdlog::shutdown(); }

}  // namespace lnfs
