"""Stress Test 5 - the CDMM gauntlet, CPython baseline.

CPython is the reference-counting model: every short-lived object is freed the
moment its name goes out of scope, the tree is one list block, and the dropped
graph is a list cleared before gc.collect() (which is what collects the
reference-counted nodes and would collect cycles).

Slots keep the per-object cost to what the model actually charges for the data.
"""

from __future__ import annotations

import gc

STACK_ITERATIONS = 200_000
POOL_ITERATIONS = 2_000
POOL_SLOTS = 8_192
TREE_NODES = 8_191
GC_NODES = 40_000


class Vec1:
    __slots__ = ("x",)

    def __init__(self, x: int = 0) -> None:
        self.x = x


class Event:
    __slots__ = ("kind", "payload")

    def __init__(self, kind: int = 0, payload: int = 0) -> None:
        self.kind = kind
        self.payload = payload


class CycleNode:
    __slots__ = ("value", "next")

    def __init__(self, value: int = 0) -> None:
        self.value = value
        self.next = None


def stack_phase() -> int:
    checksum = 0
    for i in range(STACK_ITERATIONS):
        v = Vec1(i)
        checksum += v.x
    return checksum


def pool_phase() -> int:
    events = 0
    for i in range(POOL_ITERATIONS):
        batch = [None] * POOL_SLOTS
        batch[0] = Event(i, i * 2)
        events += batch[0].kind + batch[0].payload
    return events


def arena_phase() -> int:
    tree = [0] * (TREE_NODES * 3)
    tree[0] = 5
    tree[1] = 1
    tree[2] = 2
    tree_sum = tree[0] + tree[1] + tree[2]

    leaf = Vec1(4)
    tree_sum += leaf.x
    return tree_sum


def gc_phase() -> int:
    kept: list[CycleNode] = []
    cycles = 0
    for i in range(GC_NODES):
        node = CycleNode(i)
        kept.append(node)
        cycles += node.value
    kept.clear()
    gc.collect()
    return cycles


print(stack_phase())
print(pool_phase())
print(arena_phase())
print(gc_phase())
