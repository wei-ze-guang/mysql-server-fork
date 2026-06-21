#ifndef SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H
#define SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H

#include <cstdint>
#include <string>

class THD;

namespace wzg_probe {

class TraceContext {
 public:
  void ensure_connection(THD *thd);
  void clear_connection();

  const std::string &connection_uuid() const { return m_connection_uuid; }

 private:
  std::string m_connection_uuid;
};

TraceContext &current_context();
std::uint64_t next_event_id();
std::uint64_t now_ns();

}  // namespace wzg_probe

#endif  // SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H
