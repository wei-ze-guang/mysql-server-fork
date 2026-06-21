ADD_LIBRARY(wzg_probe STATIC
  wzg_probe.cc
  wzg_trace_context.cc
  wzg_trace_event.cc
  wzg_json_writer.cc
  wzg_file_sink.cc
)

TARGET_INCLUDE_DIRECTORIES(wzg_probe
  PUBLIC
    ${CMAKE_SOURCE_DIR}
)

TARGET_LINK_LIBRARIES(wzg_probe
  mysys
)
