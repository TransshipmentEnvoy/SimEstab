#!/usr/bin/env python3

"""Tests for the frozen engine config objects and their validation.

The value assertions here are deliberately literal: they are the Python half of the
M0 decision recorded in ``doc/design_limits.md``, and a silent change to a default is
exactly what they exist to catch. If a value legitimately moves, the document and the
``sim_estab:limits`` partition move with it.
"""

import dataclasses

import pytest

from sim_estab.config import CommandPolicy, EngineConfig, HostPolicy

# --------------------------------------------------------------------------
# decided values (doc/design_limits.md 1, 2)
# --------------------------------------------------------------------------


def test_decided_defaults():
    """The shipped defaults are the values M0 decided."""
    config = EngineConfig()
    assert config.tick_rate == 30

    policy = config.command_policy
    assert policy.drain_interval_ticks == 8  # D
    assert policy.source_capacity == 64
    assert policy.host_source_capacity == 256
    assert policy.peer_source_capacity == 256


def test_capacity_is_the_only_number_an_endpoint_declares():
    """There is no drain quota: a tick executes everything stamped for it, because
    pacing closes the set before the drain runs (``design_engine_core.md`` §5.1)."""
    policy = CommandPolicy()
    assert not hasattr(policy, "source_quota")
    assert not hasattr(policy, "host_source_quota")


def test_derived_bounds_are_not_configurable():
    """``C`` is ``Σ capacity`` over an endpoint set closed at the freeze; ring ``DEPTH``
    is ``(margin + 1) × capacity`` and ``margin`` is undecided (``design_limits.md`` §7);
    the event ring is ``C × D``. None can be a construction-time value, as a field or a
    property — naming any would create an engine-wide pool, which per-endpoint rings
    avoid.
    """
    policy = CommandPolicy()
    for absent in (
        "commands_per_tick",
        "host_source_reserve",
        "event_queue_capacity",
        "ring_capacity_ticks",
        "ring_capacity",
        "ring_depth",
    ):
        assert not hasattr(
            policy, absent
        ), f"{absent} is derived at the freeze, not here"


# --------------------------------------------------------------------------
# validation (design_python_api.md 3: validate before touching native code)
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "field", ["source_capacity", "host_source_capacity", "peer_source_capacity"]
)
def test_capacities_capped_at_engine_maximum(field):
    with pytest.raises(ValueError, match="engine cap"):
        CommandPolicy(**{field: 257})


@pytest.mark.parametrize(
    "kwargs",
    [
        {"drain_interval_ticks": 0},
        {"source_capacity": 0},
        {"host_source_capacity": 0},
        {"peer_source_capacity": 0},
    ],
)
def test_command_policy_rejects_non_positive(kwargs):
    with pytest.raises(ValueError, match="must be positive"):
        CommandPolicy(**kwargs)


def test_warn_bandwidths_are_two_independent_values():
    """Separate knobs: a projection crossing is narrowed by a spec or row predicate, a
    compaction crossing by a schema (``doc/design_limits.md`` §4, §5). Retuning one must
    not move the other."""
    config = EngineConfig()
    assert config.projection_warn_bytes_per_second == 4_000_000_000
    assert config.compaction_warn_bytes_per_second == 1_000_000_000

    retuned = EngineConfig(projection_warn_bytes_per_second=8_000_000_000)
    assert retuned.projection_warn_bytes_per_second == 8_000_000_000
    assert retuned.compaction_warn_bytes_per_second == 1_000_000_000

    retuned = EngineConfig(compaction_warn_bytes_per_second=2_000_000_000)
    assert retuned.compaction_warn_bytes_per_second == 2_000_000_000
    assert retuned.projection_warn_bytes_per_second == 4_000_000_000


@pytest.mark.parametrize(
    "field",
    ["projection_warn_bytes_per_second", "compaction_warn_bytes_per_second"],
)
def test_warn_bandwidths_must_be_positive(field):
    with pytest.raises(ValueError, match="must be positive"):
        EngineConfig(**{field: 0})


def test_tick_rate_must_be_positive():
    with pytest.raises(ValueError, match="tick_rate must be positive"):
        EngineConfig(tick_rate=0)


def test_on_failed_stop_is_a_closed_set():
    with pytest.raises(ValueError, match="'raise' or 'terminate'"):
        EngineConfig(on_failed_stop="ignore")


def test_seed_may_be_absent_but_not_negative():
    assert EngineConfig(seed=None).seed is None
    assert EngineConfig(seed=0).seed == 0
    with pytest.raises(ValueError, match="seed must not be negative"):
        EngineConfig(seed=-1)


def test_capacity_entry_must_be_positive():
    """Zero is not 'uncapped'; omitting the entry is."""
    with pytest.raises(ValueError, match="omit the entry"):
        EngineConfig(capacity={"settlement": 0})


def test_host_policy_defaults():
    policy = HostPolicy()
    assert policy.shutdown_deadline == 5.0
    assert policy.mod_retry_limit == 0
    assert policy.max_inbox_size == 1024


def test_ipc_deadline_has_no_decided_value():
    """It is not a free parameter: its ceiling *is* the input delay, so it lands with
    ``input_delay_ticks`` and not before (``doc/design_limits.md`` §1.1, §7). Shipping a
    number here would be inventing the measurement it waits on."""
    assert HostPolicy().ipc_deadline is None
    assert EngineConfig().host_policy.ipc_deadline is None


def test_ipc_deadline_must_exceed_one_tick_when_set():
    """A shorter deadline can only ever detect "the other side is mid-tick", which is not
    a fault, and would expire on any hot-path round trip spanning a tick boundary."""
    with pytest.raises(ValueError, match="must exceed one tick"):
        EngineConfig(host_policy=HostPolicy(ipc_deadline=0.010))  # 10 ms < 33.3 ms

    # At the boundary exactly, and just past it.
    with pytest.raises(ValueError, match="must exceed one tick"):
        EngineConfig(tick_rate=30, host_policy=HostPolicy(ipc_deadline=1.0 / 30))
    assert (
        EngineConfig(
            tick_rate=30, host_policy=HostPolicy(ipc_deadline=0.050)
        ).host_policy.ipc_deadline
        == 0.050
    )

    # The constraint reads the configured rate, not the default one.
    EngineConfig(tick_rate=120, host_policy=HostPolicy(ipc_deadline=0.010))


def test_host_policy_validation():
    with pytest.raises(ValueError, match="shutdown_deadline must be positive"):
        HostPolicy(shutdown_deadline=0.0)
    with pytest.raises(ValueError, match="max_inbox_size must be positive"):
        HostPolicy(max_inbox_size=0)
    with pytest.raises(ValueError, match="mod_retry_limit must not be negative"):
        HostPolicy(mod_retry_limit=-1)
    with pytest.raises(ValueError, match="ipc_deadline must be positive"):
        HostPolicy(ipc_deadline=0.0)


# --------------------------------------------------------------------------
# immutability
# --------------------------------------------------------------------------


def test_config_is_frozen():
    config = EngineConfig()
    with pytest.raises(dataclasses.FrozenInstanceError):
        config.tick_rate = 60


def test_capacity_mapping_is_read_only():
    """A frozen dataclass would otherwise still hand out a mutable mapping."""
    config = EngineConfig(capacity={"settlement": 4096})
    assert config.capacity["settlement"] == 4096
    with pytest.raises(TypeError):
        config.capacity["settlement"] = 8192


def test_capacity_is_snapshotted_at_construction():
    source = {"settlement": 4096}
    config = EngineConfig(capacity=source)
    source["settlement"] = 1
    assert config.capacity["settlement"] == 4096


@pytest.mark.smoke
def test_defaults_construct_without_arguments():
    assert EngineConfig() == EngineConfig()
