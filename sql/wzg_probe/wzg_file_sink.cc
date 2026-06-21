#include "sql/wzg_probe/wzg_file_sink.h"

#include <cstdlib>
#include <cstdio>
#include <mutex>

#include "sql/wzg_probe/wzg_json_writer.h"

namespace wzg_probe {
namespace {

std::mutex sink_mutex;

}  // namespace

void emit_event(const TraceEvent &event) {
  const std::string line = to_json_line(event);

  std::lock_guard<std::mutex> guard(sink_mutex);
  const char *log_path = std::getenv("WZG_PROBE_LOG");
  FILE *file = log_path == nullptr ? nullptr : std::fopen(log_path, "a");
  if (file == nullptr) file = stderr;

  std::fwrite(line.data(), 1, line.size(), file);
  std::fwrite("\n", 1, 1, file);

  if (file != stderr) std::fclose(file);
}

}  // namespace wzg_probe
