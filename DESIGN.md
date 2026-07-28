# Execution contract

## Semantics

A predicate over `NULL` is unknown and therefore does not select the row.
Aggregates ignore null measure values. Inner joins never match null keys.
Selections preserve input row order and may be chained.

## Evidence

Every vectorized operator is compared with an independently written scalar
reference across seeded randomized tables, null placements, selectivities,
batch sizes, duplicate join keys, and empty inputs.

The benchmark must report both winning and losing regimes. Vector-at-a-time
execution has setup and selection-materialization costs; hiding its small-input
or low-selectivity crossover would make the result less useful.
