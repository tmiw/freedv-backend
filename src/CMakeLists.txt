# Configure logging
add_definitions(-DULOG_HAVE_TIME)
add_definitions(-DULOG_NO_COLOR)
# Enables the real-time-safe async front end (ulog_async.{h,cpp}). This also
# adds fields to ulog_Event, so every consumer that includes ulog.h -- here and
# in downstream projects -- must define it (see freedv-gui's src/CMakeLists.txt).
add_definitions(-DULOG_ASYNC)

# Including 3rdparty folder in include path in order for websocketpp to be correctly found
include_directories(3rdparty)

add_subdirectory(3rdparty)
add_subdirectory(os)
add_subdirectory(pipeline)
add_subdirectory(reporting)
add_subdirectory(util)
