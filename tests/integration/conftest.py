#!/usr/bin/env python3
import os
import pathlib

import pytest

import harness


def _required_path(variable: str) -> str:
    value = os.environ.get(variable)
    if not value:
        pytest.exit(
            f"{variable} is not set; run the tests with `just pytest`",
            returncode=pytest.ExitCode.USAGE_ERROR,
        )
    if not pathlib.Path(value).is_file():
        pytest.exit(f"{variable}={value} does not exist", returncode=pytest.ExitCode.USAGE_ERROR)
    return value


@pytest.fixture(scope="session")
def log_shim() -> harness.LogShim:
    return harness.LogShim(_required_path("DOCVIEWER_LOG_SHIM"))


@pytest.fixture(scope="session")
def core(log_shim: harness.LogShim) -> harness.Core:
    return harness.Core(_required_path("DOCVIEWER_CORE"), log_shim)
