# nodio-dap.exe — Debug Adapter Protocol over stdio. Links the engine so it
# can host the debuggee session (breakpoints, stepping, stack, variables).
add_executable(nodio-dap main.cpp)
target_link_libraries(nodio-dap PRIVATE nodio_engine)
set_target_properties(nodio-dap PROPERTIES LINK_FLAGS "/STACK:16777216")
