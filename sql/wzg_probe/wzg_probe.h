#ifndef SQL_WZG_PROBE_WZG_PROBE_H
#define SQL_WZG_PROBE_WZG_PROBE_H

#include <cstdint>
#include <cstddef>
#include <string>

class THD;

namespace wzg_probe {

class Event {
 public:
  Event(THD *thd, const char *event_name, const char *event_phase);
  ~Event();

  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;
  Event(Event &&) = delete;
  Event &operator=(Event &&) = delete;

  Event &field(const char *key, const char *value);
  Event &field(const char *key, const std::string &value);
  Event &field(const char *key, std::int64_t value);
  Event &field(const char *key, std::uint64_t value);
  Event &field(const char *key, bool value);
  Event &message(const char *value);
  Event &command(const char *value);
  Event &sql_command(const char *value);

  void emit();

 private:
  class Impl;
  Impl *m_impl;
};

class Scope {
 public:
  Scope(THD *thd, const char *event_name);
  ~Scope();

  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  Scope(Scope &&) = delete;
  Scope &operator=(Scope &&) = delete;

  Scope &field(const char *key, const char *value);
  Scope &field(const char *key, const std::string &value);
  Scope &field(const char *key, std::int64_t value);
  Scope &field(const char *key, std::uint64_t value);
  Scope &field(const char *key, bool value);
  Scope &message(const char *value);
  Scope &command(const char *value);
  Scope &sql_command(const char *value);

 private:
  Event m_event;
};

void on_connection_start(THD *thd);
void on_connection_end(THD *thd);
void set_raw_sql(const char *sql, std::size_t length);
void clear_raw_sql();

}  // namespace wzg_probe

#define WZG_PROBE_EVENT(thd, event_name) \
  wzg_probe::Event((thd), (event_name), "instant")

#define WZG_PROBE_SCOPE(thd, event_name) \
  wzg_probe::Scope wzg_probe_scope_##__LINE__((thd), (event_name))

#endif  // SQL_WZG_PROBE_WZG_PROBE_H
