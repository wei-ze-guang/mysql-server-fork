#include "sql/wzg_probe/wzg_file_sink.h"

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
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fwrite("\n", 1, 1, stderr);
}

}  // namespace wzg_probe
