#ifndef SQL_WZG_PROBE_WZG_FILE_SINK_H
#define SQL_WZG_PROBE_WZG_FILE_SINK_H

#include "sql/wzg_probe/wzg_trace_event.h"

namespace wzg_probe {

void emit_event(const TraceEvent &event);

}  // namespace wzg_probe

#endif  // SQL_WZG_PROBE_WZG_FILE_SINK_H
