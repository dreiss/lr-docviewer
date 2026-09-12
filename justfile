build_root := env_var_or_default("BUILD_ROOT", "build")
jobs := env_var_or_default("JOBS", `nproc`)

# List available actions.
list:
  @{{ just_executable() }} --list

# List configurable variables and their current values.
vars:
  @{{ just_executable() }} --evaluate

# Build the optimized core.
build: release

[private]
_build type directory sanitizers:
  cmake -S . -B {{build_root}}/{{directory}} \
    -DCMAKE_BUILD_TYPE={{type}} \
    -DDOCVIEWER_ENABLE_CLANG_TIDY=ON \
    -DDOCVIEWER_ENABLE_SANITIZERS={{sanitizers}}
  cmake --build {{build_root}}/{{directory}} --parallel {{jobs}}

# Build the optimized core.
release: (_build "Release" "release" "OFF")

# Build the core with debug information.
debug: (_build "Debug" "debug" "OFF")

# Build the core with ASan and UBSan.
sanitize: (_build "Debug" "sanitize" "ON")

# Format the project C sources and headers.
format:
  clang-format -i src/*.c src/*.h

# Check formatting without changing files.
format-check:
  clang-format --dry-run --Werror src/*.c src/*.h
