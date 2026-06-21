#ifndef SQL_WZG_PROBE_WZG_TRACE_EVENT_H
#define SQL_WZG_PROBE_WZG_TRACE_EVENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace wzg_probe {

struct Field {
  std::string key;
  std::string value;
};

struct TraceEvent {
  std::string schema{"wzg.raw.v1"};
  std::uint64_t event_id{0};
  std::uint64_t ts_ns{0};
  std::uint64_t duration_ns{0};

  std::string event_name;
  std::string event_phase;
  std::string event_level{"info"};

  std::uint64_t thread_id{0};
  std::uint64_t connection_id{0};
  std::string connection_uuid;
  std::uint64_t query_id{0};

  std::string user;
  std::string host;
  std::string db;
  std::string command;
  std::string sql_command;
  std::string query;

  bool error{false};
  std::uint64_t error_code{0};
  std::uint64_t warning_count{0};

  std::vector<Field> fields;
};

}  // namespace wzg_probe

#endif  // SQL_WZG_PROBE_WZG_TRACE_EVENT_H
