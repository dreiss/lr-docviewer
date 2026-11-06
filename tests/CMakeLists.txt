add_library(log_shim SHARED integration/log_shim.c)
target_compile_features(log_shim PRIVATE c_std_11)
target_compile_options(log_shim PRIVATE ${DOCVIEWER_WARNINGS})
target_include_directories(log_shim PRIVATE ${PROJECT_SOURCE_DIR}/vendor/libretro-common/include)
