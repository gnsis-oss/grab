# Vendored l0 libraries

Source: ~/ws/l0 monorepo, rev a8693bf4, copied 2026-07-13.
Libraries: tag (t1), web (t2, syntax/ excluded), walk (t3), heap (t2), out (t0).
grab-authored shims: log/writer.hpp (no-op
`logger::{tag,trace,error,nominal,verbose,debug}`; the copied trees currently
call `logger::{tag,trace,error}`).

Required patches to upstream files (verified defects; see plan Task 1 / P1.2):
- walk/diff.hpp: endpoint key `(hash(from)*1'000'003) ^ hash(to)` collides
  (e.g. pairs (1,2) and (3,2262152) both fold to 1000001), silently dropping
  edge deltas. Patch applied here: key edges by an equality-aware endpoint set,
  not the hash fold. grab derives widget events from this diff, so the collision
  is load-bearing; the vendor smoke test covers the collision.
- walk/graft.hpp: the fresh-id remap is internal and attachment edges are
  created as `E{}`. Patch to return the old→new id map and take an explicit
  attachment RelationId (needed by the TreeStore cross-process embed path).
Any further edit must be listed here per file.
Internal-only: these namespaces (tag/web/walk/heap/out/logger) must never appear
in include/grab/ (CI-checked).
