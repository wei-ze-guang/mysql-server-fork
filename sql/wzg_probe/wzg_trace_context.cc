#include "sql/wzg_probe/wzg_trace_context.h"

#include <atomic>
#include <chrono>
#include <sstream>

#include "sql/sql_class.h"

namespace wzg_probe {
namespace {

thread_local TraceContext tls_context;
std::atomic<std::uint64_t> global_event_id{1};
std::atomic<std::uint64_t> global_connection_seq{1};

}  // namespace

void TraceContext::ensure_connection(THD *thd) {
  if (!m_connection_uuid.empty()) return;

  const std::uint64_t seq = global_connection_seq.fetch_add(1);
  const std::uint64_t mysql_thread_id = thd ? thd->thread_id() : 0;

  std::ostringstream out;
  out << "conn-" << mysql_thread_id << "-" << seq;
  m_connection_uuid = out.str();
}

void TraceContext::clear_connection() { m_connection_uuid.clear(); }

TraceContext &current_context() { return tls_context; }

std::uint64_t next_event_id() { return global_event_id.fetch_add(1); }

std::uint64_t now_ns() {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

}  // namespace wzg_probe
