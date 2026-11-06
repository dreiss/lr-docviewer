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

c_sources := "src/*.c src/*.h tests/integration/*.c"

# Format the project C and Python sources.
format:
  clang-format -i {{c_sources}}
  .venv/bin/ruff format

# Check formatting without changing files.
format-check:
  clang-format --dry-run --Werror {{c_sources}}
  .venv/bin/ruff format --check

# Lint the Python sources.
lint:
  .venv/bin/ruff check

# Create .venv with the Python packages that the integration tests need.
venv:
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt

[private]
_pytest directory *args:
  DOCVIEWER_CORE={{build_root}}/{{directory}}/docviewer_libretro.so \
  DOCVIEWER_LOG_SHIM={{build_root}}/{{directory}}/tests/liblog_shim.so \
    .venv/bin/python tests/integration/run.py {{args}}

# Build the debug core and run the tests, passing arguments to pytest.
pytest *args: debug (_pytest "debug" args)

# Build the sanitizer core and run the tests, passing arguments to pytest.
pytest-sanitize *args: sanitize (_pytest "sanitize" args)
