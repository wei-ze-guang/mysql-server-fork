#include "sql/wzg_probe/wzg_json_writer.h"

#include <sstream>

namespace wzg_probe {
namespace {

void append_json_string(std::ostringstream &out, const std::string &value) {
  out << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          const char hex[] = "0123456789abcdef";
          out << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
        } else {
          out << ch;
        }
        break;
    }
  }
  out << '"';
}

void append_key(std::ostringstream &out, const char *key) {
  append_json_string(out, key);
  out << ':';
}

void append_string_field(std::ostringstream &out, const char *key,
                         const std::string &value, bool &first) {
  if (!first) out << ',';
  first = false;
  append_key(out, key);
  append_json_string(out, value);
}

void append_uint_field(std::ostringstream &out, const char *key,
                       std::uint64_t value, bool &first) {
  if (!first) out << ',';
  first = false;
  append_key(out, key);
  out << value;
}

}  // namespace

std::string to_json_line(const TraceEvent &event) {
  std::ostringstream out;
  bool first = true;

  out << '{';
  append_uint_field(out, "event_id", event.event_id, first);
  append_uint_field(out, "ts_ns", event.ts_ns, first);
  append_uint_field(out, "duration_ns", event.duration_ns, first);
  append_string_field(out, "event_name", event.event_name, first);
  append_string_field(out, "event_phase", event.event_phase, first);
  append_string_field(out, "message", event.message, first);
  append_uint_field(out, "thread_id", event.thread_id, first);
  append_uint_field(out, "connection_id", event.connection_id, first);
  append_string_field(out, "connection_uuid", event.connection_uuid, first);
  append_uint_field(out, "query_id", event.query_id, first);
  append_string_field(out, "db", event.db, first);
  append_string_field(out, "command", event.command, first);
  append_string_field(out, "sql_command", event.sql_command, first);
  append_string_field(out, "raw_sql", event.raw_sql, first);
  append_string_field(out, "query", event.query, first);

  if (!first) out << ',';
  append_key(out, "fields");
  out << '{';
  bool first_field = true;
  for (const Field &field : event.fields) {
    if (!first_field) out << ',';
    first_field = false;
    append_key(out, field.key.c_str());
    append_json_string(out, field.value);
  }
  out << '}';

  out << '}';
  return out.str();
}

}  // namespace wzg_probe
