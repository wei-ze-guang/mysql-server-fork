#include "sql/wzg_probe/wzg_trace_context.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <sstream>
#include <string>

#include "sql/sql_class.h"

namespace wzg_probe {
namespace {

thread_local TraceContext tls_context;
std::atomic<std::uint64_t> global_event_id{1};
std::atomic<std::uint64_t> global_connection_seq{1};
constexpr std::size_t kMaxRawSqlBytes = 4096;

std::string normalize_sql_for_filter(const char *sql, std::size_t length) {
  std::string out;
  out.reserve(length);

  bool last_was_space = true;
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned char ch = static_cast<unsigned char>(sql[i]);
    if (std::isspace(ch)) {
      if (!last_was_space) {
        out.push_back(' ');
        last_was_space = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(ch)));
    last_was_space = false;
  }

  while (!out.empty() && (out.back() == ' ' || out.back() == ';')) {
    out.pop_back();
  }
  return out;
}

bool starts_with(const std::string &value, const char *prefix) {
  const std::string_view view(value);
  return view.starts_with(prefix);
}

}  // namespace

void TraceContext::ensure_connection(THD *thd) {
  if (!m_connection_uuid.empty()) return;

  const std::uint64_t seq = global_connection_seq.fetch_add(1);
  const std::uint64_t mysql_thread_id = thd ? thd->thread_id() : 0;

  std::ostringstream out;
  out << "conn-" << mysql_thread_id << "-" << seq;
  m_connection_uuid = out.str();
}

void TraceContext::clear_connection() {
  m_connection_uuid.clear();
  clear_raw_sql();
}

void TraceContext::set_raw_sql(const char *sql, std::size_t length) {
  if (sql == nullptr || length == 0) {
    m_raw_sql.clear();
    m_suppress_current_sql = false;
    return;
  }
  m_suppress_current_sql = should_suppress_sql(sql, length);
  if (length > kMaxRawSqlBytes) length = kMaxRawSqlBytes;
  m_raw_sql.assign(sql, length);
}

void TraceContext::clear_raw_sql() {
  m_raw_sql.clear();
  m_suppress_current_sql = false;
}

void TraceContext::set_suppress_current_sql(bool value) {
  m_suppress_current_sql = value;
}

TraceContext &current_context() { return tls_context; }

std::uint64_t next_event_id() { return global_event_id.fetch_add(1); }

std::uint64_t now_ns() {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

void set_raw_sql(const char *sql, std::size_t length) {
  current_context().set_raw_sql(sql, length);
}

void clear_raw_sql() { current_context().clear_raw_sql(); }

bool should_suppress_sql(const char *sql, std::size_t length) {
  if (sql == nullptr || length == 0) return false;

  const std::string normalized = normalize_sql_for_filter(sql, length);
  if (normalized.empty()) return false;

  if (starts_with(normalized, "show databases") ||
      starts_with(normalized, "show schemas") ||
      starts_with(normalized, "show tables") ||
      starts_with(normalized, "show full tables") ||
      starts_with(normalized, "show table status") ||
      starts_with(normalized, "show columns") ||
      starts_with(normalized, "show fields") ||
      starts_with(normalized, "show keys") ||
      starts_with(normalized, "show indexes") ||
      starts_with(normalized, "show index") ||
      starts_with(normalized, "show variables") ||
      starts_with(normalized, "show global variables") ||
      starts_with(normalized, "show session variables") ||
      starts_with(normalized, "show warnings") ||
      starts_with(normalized, "show engines") ||
      starts_with(normalized, "show character set") ||
      starts_with(normalized, "show charset") ||
      starts_with(normalized, "show collation") ||
      starts_with(normalized, "show grants") ||
      starts_with(normalized, "show create table") ||
      starts_with(normalized, "show create database")) {
    return true;
  }

  if (starts_with(normalized, "select database()") ||
      starts_with(normalized, "select schema()") ||
      starts_with(normalized, "select version()") ||
      starts_with(normalized, "select connection_id()") ||
      starts_with(normalized, "select current_user()") ||
      starts_with(normalized, "select user()") ||
      starts_with(normalized, "select @@") ||
      starts_with(normalized, "select @@session.") ||
      starts_with(normalized, "select @@global.")) {
    return true;
  }

  if (starts_with(normalized, "set names") ||
      starts_with(normalized, "set character_set_results") ||
      starts_with(normalized, "set autocommit") ||
      starts_with(normalized, "set sql_select_limit") ||
      starts_with(normalized, "set net_write_timeout") ||
      starts_with(normalized, "set session transaction") ||
      starts_with(normalized, "use ")) {
    return true;
  }

  return false;
}

bool suppress_current_sql() {
  return current_context().suppress_current_sql();
}

}  // namespace wzg_probe
