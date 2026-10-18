# nodio-lsp.exe — Language Server Protocol over stdio. Links the engine so it
# can reuse the lexer/parser for diagnostics, completion and symbols.
add_executable(nodio-lsp main.cpp)
target_link_libraries(nodio-lsp PRIVATE nodio_engine)
set_target_properties(nodio-lsp PROPERTIES LINK_FLAGS "/STACK:16777216")
