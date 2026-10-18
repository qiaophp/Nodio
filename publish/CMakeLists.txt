# nodio_publish — binary-safe resource embedder, .aproj/.nproj project rules,
# import-closure collection and PE icon/version/manifest/UAC stamping.
add_library(nodio_publish STATIC
    publish.cpp
    publish.h
    payload.h
)
target_link_libraries(nodio_publish PUBLIC nodio_engine)
