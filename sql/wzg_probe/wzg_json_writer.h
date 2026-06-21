#ifndef SQL_WZG_PROBE_WZG_JSON_WRITER_H
#define SQL_WZG_PROBE_WZG_JSON_WRITER_H

#include <string>

#include "sql/wzg_probe/wzg_trace_event.h"

namespace wzg_probe {

std::string to_json_line(const TraceEvent &event);

}  // namespace wzg_probe

#endif  // SQL_WZG_PROBE_WZG_JSON_WRITER_H
