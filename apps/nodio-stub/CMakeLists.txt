# apps/nodio-stub/CMakeLists.txt — minimal standalone runtime stub.
add_executable(nodio-stub
    main.cpp
    host.rc
)
target_link_libraries(nodio-stub PRIVATE nodio_engine)

if(MSVC)
    target_link_options(nodio-stub PRIVATE "/MANIFEST:NO")
endif()

set_target_properties(nodio-stub PROPERTIES LINK_FLAGS "/STACK:16777216")
