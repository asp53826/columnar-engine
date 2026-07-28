# columnar-engine

A dependency-free C++17 vector-at-a-time analytical query engine.

The correctness contract comes before the benchmark:

- typed contiguous columns with explicit null bitmaps;
- composable selection vectors with SQL-style null filtering;
- batched numeric predicates and fused multi-predicate execution;
- hash aggregation and inner hash joins;
- a deliberately separate tuple-at-a-time reference executor;
- randomized differential tests that require both executors to agree.

Measured crossover points and operator-level results will be published after
the implementation passes on macOS and Linux.
