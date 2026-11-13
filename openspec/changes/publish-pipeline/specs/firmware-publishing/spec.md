## ADDED Requirements

### Requirement: One publish commit per run
The firmware build SHALL publish all boards of a run in a single commit to `main`, so parallel board jobs cannot race on the push.

#### Scenario: A run builds all boards
- **WHEN** a push to `main` builds every board
- **THEN** exactly one "Publish" commit lands, carrying every board's binaries and the regenerated manifest
<!-- unproved -->
