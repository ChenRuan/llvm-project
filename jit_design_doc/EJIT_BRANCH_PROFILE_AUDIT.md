# EJIT Branch Profile Audit

## Purpose

`EJIT_SRE_PGO_BRANCH_AUDIT` is an experimental, default-off diagnostic for
finding EJIT annotations whose extracted modules show little branch-profile
optimization opportunity. It reuses the normal online-PGO Tier-1 edge counters
and adds no second instrumentation path.

The build-script switch enables the existing EJIT diagnostics as well. If the
CMake option is enabled directly while diagnostics are compiled out, the audit
performs no IR scan and has no runtime cost.

The audit runs after `PGOInstrumentationUse` restores branch weights and before
ICP or module inlining changes the original function boundaries. It reports:

- root execution count, defined functions, and IR instruction count in each
  entry module;
- conditional branch sites and sites carrying valid profile weights;
- strongly biased sites, where one successor has at least 95% of the weight;
- balanced sites, where no successor has more than 60% of the weight;
- zero-count edges.

The INFO line is one summary per profiled EJIT entry module. DEBUG adds one line
per defined function. These numbers are evidence for ranking candidates; they
do not by themselves prove that an annotation causes a performance regression.
A final A/B run with the candidate annotation disabled is still required.

## Build and run

```sh
./build.sh release aarch64_be \
  --sre-taskpool --sre-shared-taskpool \
  --sre-taskpool-no-reclaim --sre-shared-code-pointers \
  --sre-pgo-branch-audit \
  --sre-pgo-max-concurrent-profiles=4 --stats
```

Start the online-PGO worker as usual, call every workload path whose annotations
must be audited, and wait for Tier-2 publication. Set the EJIT diagnostic level
to DEBUG when per-function lines are needed.

## Coverage boundary

Tier-1 instruments every defined function in the extracted module, so callees
are included even when they are not separate `ejit_entry` roots. The scheduler
eventually profiles every *executed* EJIT entry specialization in batches; the
concurrency limit only controls how many are active at once. A never-called
entry or specialization has no dynamic profile and cannot appear in this audit.

The same source annotation may create multiple cache keys for different period
dimensions. Each executed key is audited separately because its branch behavior
can differ.
