#include "sql/wzg_probe/wzg_probe.h"

#include <sstream>

#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_class.h"
#include "sql/wzg_probe/wzg_file_sink.h"
#include "sql/wzg_probe/wzg_trace_context.h"

namespace wzg_probe {
namespace {

constexpr std::size_t kMaxQueryBytes = 4096;

std::string copy_lex_cstring(const LEX_CSTRING &value,
                             std::size_t max_length = 0) {
  if (value.str == nullptr || value.length == 0) return {};
  const std::size_t length =
      max_length == 0 || value.length <= max_length ? value.length : max_length;
  return std::string(value.str, length);
}

std::string to_string(std::int64_t value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string to_string(std::uint64_t value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

void snapshot_thd(TraceEvent *event, THD *thd) {
  if (event == nullptr) return;

  TraceContext &ctx = current_context();
  ctx.ensure_connection(thd);
  event->connection_uuid = ctx.connection_uuid();

  if (thd == nullptr) return;

  event->thread_id = thd->thread_id();
  event->connection_id = thd->thread_id();
  event->query_id = thd->query_id;
  event->db = copy_lex_cstring(thd->db());
  event->query = copy_lex_cstring(thd->query(), kMaxQueryBytes);
  event->raw_sql = ctx.raw_sql();
  event->error = thd->is_error();
  event->warning_count = thd->get_stmt_da()->current_statement_cond_count();
  if (thd->get_stmt_da()->is_error())
    event->error_code = thd->get_stmt_da()->mysql_errno();

  Security_context *security_context = thd->security_context();
  if (security_context != nullptr) {
    event->user = copy_lex_cstring(security_context->user());
    event->host = copy_lex_cstring(security_context->host_or_ip());
  }
}

}  // namespace

class Event::Impl {
 public:
  Impl(THD *thd, const char *event_name, const char *event_phase)
      : m_start_ns(now_ns()) {
    m_event.event_id = next_event_id();
    m_event.ts_ns = m_start_ns;
    m_event.event_name = event_name == nullptr ? "" : event_name;
    m_event.event_phase = event_phase == nullptr ? "instant" : event_phase;
    snapshot_thd(&m_event, thd);
  }

  Event &field(Event *owner, const char *key, const std::string &value) {
    if (key != nullptr) m_event.fields.push_back({key, value});
    return *owner;
  }

  Event &message(Event *owner, const char *value) {
    m_event.message = value == nullptr ? "" : value;
    return *owner;
  }

  Event &command(Event *owner, const char *value) {
    m_event.command = value == nullptr ? "" : value;
    return *owner;
  }

  Event &sql_command(Event *owner, const char *value) {
    m_event.sql_command = value == nullptr ? "" : value;
    return *owner;
  }

  void emit() {
    if (m_emitted) return;
    if (m_event.event_phase == "end") {
      const std::uint64_t end_ns = now_ns();
      m_event.duration_ns = end_ns >= m_start_ns ? end_ns - m_start_ns : 0;
    }
    emit_event(m_event);
    m_emitted = true;
  }

 private:
  TraceEvent m_event;
  std::uint64_t m_start_ns{0};
  bool m_emitted{false};
};

Event::Event(THD *thd, const char *event_name, const char *event_phase)
    : m_impl(new Impl(thd, event_name, event_phase)) {}

Event::~Event() { delete m_impl; }

Event &Event::field(const char *key, const char *value) {
  return m_impl->field(this, key, value == nullptr ? "" : value);
}

Event &Event::field(const char *key, const std::string &value) {
  return m_impl->field(this, key, value);
}

Event &Event::field(const char *key, std::int64_t value) {
  return m_impl->field(this, key, to_string(value));
}

Event &Event::field(const char *key, std::uint64_t value) {
  return m_impl->field(this, key, to_string(value));
}

Event &Event::field(const char *key, bool value) {
  return m_impl->field(this, key, value ? "true" : "false");
}

Event &Event::message(const char *value) { return m_impl->message(this, value); }

Event &Event::command(const char *value) { return m_impl->command(this, value); }

Event &Event::sql_command(const char *value) {
  return m_impl->sql_command(this, value);
}

void Event::emit() { m_impl->emit(); }

Scope::Scope(THD *thd, const char *event_name)
    : m_event(thd, event_name, "end") {}

Scope::~Scope() { m_event.emit(); }

Scope &Scope::field(const char *key, const char *value) {
  m_event.field(key, value);
  return *this;
}

Scope &Scope::field(const char *key, const std::string &value) {
  m_event.field(key, value);
  return *this;
}

Scope &Scope::field(const char *key, std::int64_t value) {
  m_event.field(key, value);
  return *this;
}

Scope &Scope::field(const char *key, std::uint64_t value) {
  m_event.field(key, value);
  return *this;
}

Scope &Scope::field(const char *key, bool value) {
  m_event.field(key, value);
  return *this;
}

Scope &Scope::message(const char *value) {
  m_event.message(value);
  return *this;
}

Scope &Scope::command(const char *value) {
  m_event.command(value);
  return *this;
}

Scope &Scope::sql_command(const char *value) {
  m_event.sql_command(value);
  return *this;
}

void on_connection_start(THD *thd) {
  current_context().ensure_connection(thd);
  Event(thd, "connection.start", "instant")
      .message("连接线程开始处理新的客户端连接")
      .field("note", "只记录连接上下文，不做复杂认证细节")
      .emit();
}

void on_connection_end(THD *thd) {
  Event(thd, "connection.end", "instant")
      .message("连接线程结束处理客户端连接")
      .field("note", "连接生命周期结束，清理 WZG 线程上下文")
      .emit();
  current_context().clear_connection();
}

const std::string &raw_sql() { return current_context().raw_sql(); }

}  // namespace wzg_probe
