"""Tests for the apiexec Python binding."""

import pytest
from apiexec import Stream, ApiExecError


def test_unknown_adapter():
    with pytest.raises(ApiExecError):
        Stream("nonexistent", '{"base_url": "http://x"}')


def test_invalid_json():
    with pytest.raises(ApiExecError):
        Stream("generic_rest", "not json")


def test_create_valid():
    s = Stream("generic_rest", '{"base_url": "http://localhost:9999"}',
               '{"prefetch_depth": 0}')
    assert s.has_next()
    s.close()


def test_cancel():
    s = Stream("generic_rest", '{"base_url": "http://localhost:9999"}',
               '{"prefetch_depth": 0}')
    s.cancel()
    assert not s.has_next()
    s.close()


def test_close_idempotent():
    s = Stream("generic_rest", '{"base_url": "http://localhost:9999"}')
    s.close()
    s.close()  # should not crash


def test_context_manager():
    with Stream("generic_rest", '{"base_url": "http://localhost:9999"}',
                '{"prefetch_depth": 0}') as s:
        assert s.has_next()


def test_network_error():
    s = Stream("generic_rest", '{"base_url": "http://127.0.0.1:1"}',
               '{"max_retries": 0, "prefetch_depth": 0}')
    with pytest.raises(ApiExecError) as exc_info:
        s.next_batch()
    assert exc_info.value.code == -5  # NETWORK
    s.close()
