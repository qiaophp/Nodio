# nodio.exe — the standalone host. Roles: run a script (NODIO_RUN / a path
# argument), the F5 + regression harness, the console mini-debugger
# (NODIO_DEBUG / NODIO_BREAK), the parse sweep (NODIO_CHECK), the publisher
# (NODIO_PUBLISH) and the published-exe payload loader.
add_executable(nodio
    main.cpp
    host.rc
)
target_link_libraries(nodio PRIVATE nodio_publish)

# host.rc embeds the manifest + version info as real PE resources at the ids
# the Windows loader reads. The VS generator also emits its own default
# manifest and merges it at link time (CVT1100 duplicate on RT_MANIFEST id 1),
# so disable that merge.
if(MSVC)
    target_link_options(nodio PRIVATE "/MANIFEST:NO")
endif()

# 16 MB stack reserve: the tree-walking interpreter nests many C++ frames per
# aardio call, so the default 1 MB reserve crashes a deep-but-legit aardio
# recursion (~70 frames) before the engine's depth guard can fire.
set_target_properties(nodio PROPERTIES LINK_FLAGS "/STACK:16777216")
