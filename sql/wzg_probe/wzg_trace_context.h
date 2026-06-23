#ifndef SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H
#define SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H

#include <cstdint>
#include <cstddef>
#include <string>

class THD;

namespace wzg_probe {

class TraceContext {
 public:
  void ensure_connection(THD *thd);
  void clear_connection();
  void set_raw_sql(const char *sql, std::size_t length);
  void clear_raw_sql();
  void set_suppress_current_sql(bool value);

  const std::string &connection_uuid() const { return m_connection_uuid; }
  const std::string &raw_sql() const { return m_raw_sql; }
  bool suppress_current_sql() const { return m_suppress_current_sql; }

 private:
  std::string m_connection_uuid;
  std::string m_raw_sql;
  bool m_suppress_current_sql{false};
};

TraceContext &current_context();
std::uint64_t next_event_id();
std::uint64_t now_ns();
void set_raw_sql(const char *sql, std::size_t length);
void clear_raw_sql();
bool should_suppress_sql(const char *sql, std::size_t length);
bool suppress_current_sql();

}  // namespace wzg_probe

#endif  // SQL_WZG_PROBE_WZG_TRACE_CONTEXT_H
