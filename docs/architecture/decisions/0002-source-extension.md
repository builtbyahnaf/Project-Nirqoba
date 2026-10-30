# ADR 0002: Official Source Extension

## Status
Accepted

## Context
Across early design notes and ROADMAP documents, both `.nrqba` and `.nrqba` appeared. A single canonical source extension is needed for tooling, build systems, syntax highlighters, and compiler diagnostics.

## Decision
Use `.nrqba` as the official and only accepted source file extension for Nirqoba programs.
Passing any other extension produces diagnostic `NRQ5007: source file must have extension '.nrqba'`.
