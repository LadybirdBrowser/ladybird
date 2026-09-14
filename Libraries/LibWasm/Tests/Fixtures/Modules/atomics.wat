;; Threads-proposal atomic instructions on a non-shared memory.
(module
  (memory (export "memory") 1)

  (func (export "i32_load") (param i32) (result i32) (i32.atomic.load (local.get 0)))
  (func (export "i32_store") (param i32 i32) (i32.atomic.store (local.get 0) (local.get 1)))
  (func (export "i32_load8_u") (param i32) (result i32) (i32.atomic.load8_u (local.get 0)))
  (func (export "i32_store8") (param i32 i32) (i32.atomic.store8 (local.get 0) (local.get 1)))
  (func (export "i32_load16_u") (param i32) (result i32) (i32.atomic.load16_u (local.get 0)))
  (func (export "i64_load") (param i32) (result i64) (i64.atomic.load (local.get 0)))
  (func (export "i64_store") (param i32 i64) (i64.atomic.store (local.get 0) (local.get 1)))
  (func (export "i64_load32_u") (param i32) (result i64) (i64.atomic.load32_u (local.get 0)))
  (func (export "i64_store16") (param i32 i64) (i64.atomic.store16 (local.get 0) (local.get 1)))

  (func (export "i32_rmw_add") (param i32 i32) (result i32) (i32.atomic.rmw.add (local.get 0) (local.get 1)))
  (func (export "i32_rmw_sub") (param i32 i32) (result i32) (i32.atomic.rmw.sub (local.get 0) (local.get 1)))
  (func (export "i32_rmw_and") (param i32 i32) (result i32) (i32.atomic.rmw.and (local.get 0) (local.get 1)))
  (func (export "i32_rmw_or") (param i32 i32) (result i32) (i32.atomic.rmw.or (local.get 0) (local.get 1)))
  (func (export "i32_rmw_xor") (param i32 i32) (result i32) (i32.atomic.rmw.xor (local.get 0) (local.get 1)))
  (func (export "i32_rmw_xchg") (param i32 i32) (result i32) (i32.atomic.rmw.xchg (local.get 0) (local.get 1)))
  (func (export "i32_rmw8_add_u") (param i32 i32) (result i32) (i32.atomic.rmw8.add_u (local.get 0) (local.get 1)))
  (func (export "i32_rmw16_sub_u") (param i32 i32) (result i32) (i32.atomic.rmw16.sub_u (local.get 0) (local.get 1)))
  (func (export "i64_rmw_add") (param i32 i64) (result i64) (i64.atomic.rmw.add (local.get 0) (local.get 1)))
  (func (export "i64_rmw16_xchg_u") (param i32 i64) (result i64) (i64.atomic.rmw16.xchg_u (local.get 0) (local.get 1)))
  (func (export "i64_rmw32_or_u") (param i32 i64) (result i64) (i64.atomic.rmw32.or_u (local.get 0) (local.get 1)))

  (func (export "i32_cmpxchg") (param i32 i32 i32) (result i32) (i32.atomic.rmw.cmpxchg (local.get 0) (local.get 1) (local.get 2)))
  (func (export "i32_cmpxchg8_u") (param i32 i32 i32) (result i32) (i32.atomic.rmw8.cmpxchg_u (local.get 0) (local.get 1) (local.get 2)))
  (func (export "i64_cmpxchg") (param i32 i64 i64) (result i64) (i64.atomic.rmw.cmpxchg (local.get 0) (local.get 1) (local.get 2)))
  (func (export "i64_cmpxchg32_u") (param i32 i64 i64) (result i64) (i64.atomic.rmw32.cmpxchg_u (local.get 0) (local.get 1) (local.get 2)))

  (func (export "notify") (param i32 i32) (result i32) (memory.atomic.notify (local.get 0) (local.get 1)))
  (func (export "wait32") (param i32 i32 i64) (result i32) (memory.atomic.wait32 (local.get 0) (local.get 1) (local.get 2)))
  (func (export "wait64") (param i32 i64 i64) (result i32) (memory.atomic.wait64 (local.get 0) (local.get 1) (local.get 2)))
  (func (export "fence") (atomic.fence))

  (func (export "i32_load_offset") (param i32) (result i32) (i32.atomic.load offset=4 (local.get 0)))
  (func (export "plain_i32_load") (param i32) (result i32) (i32.load (local.get 0)))
)
