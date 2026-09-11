# Native resolved execution settlement

`BacktestEngine::settle_resolved_execution` is a protected C++ extension for
trusted matching adapters. It applies an already admitted, matched execution
to the engine's existing physical lot book and emits the resulting close and
open observations. It adds no second position ledger or persistent selector.

The caller supplies an `execution::Action` and `execution::Fill` from
`<pineforge/execution.hpp>`. These values describe immediate effects:

| Action | Effect |
| --- | --- |
| `Flatten{}` | Close every physical lot. |
| `order_action::Reduce{units}` | Close up to a finite, nonnegative quantity in FIFO order; never open or reverse. |
| `order_action::Transact{signed_units}` | Buy positive units or sell negative units; reduce opposite exposure first, then open any remainder. |

`Fill` carries the resolved finite price, order ID, comment, incarnation and
an optional total commission in account currency. Zero and negative prices
are representable; the caller decides which prices its instrument permits.
Settlement does not apply another tick rounding, slippage, lot step,
pyramiding cap or affordability decision.

## Physical lots and quantities

FIFO allocation retains surviving lot identities, entry prices, paid costs
and proportional excursions. The surviving physical quantities determine the
resulting position and weighted entry price. For example, reducing lots
`{0.1, 0.2}` by `0.1` leaves the existing `0.2` lot, even when regrouping
binary64 arithmetic as `aggregate_before - reduction` differs by one ulp.

A finite quantity request can leave a positive floating residual. No native
epsilon silently discards it. `Flatten` explicitly closes all lots and avoids
using rounded aggregate equality to mean a whole-book close. Quantities whose
changes cannot be represented are refused before settlement.

## Costs and marked equity

Every opening lot receives its paid entry commission before its entry
observation is emitted. Partial closes allocate this stored payment in
proportion to closed quantity for every commission type. The survivor keeps
the unconsumed cost. Later fee schedule or FX changes do not rewrite a payment
already made.

The current execution is quoted separately. Percent and per-contract charges
follow each physical quantity. A cash-per-order charge belongs to the entire
native execution, including both legs of a reversal; it is allocated across
the close/open effects with the final effect receiving the floating residue.
An explicit `Fill::commission_account` overrides the modeled total and can
represent a zero-fee waiver or a negative rebate. A nonfinite quote, or a
nonzero quote attached to a no-effect quantity, is refused.

For example, with a cash ticket of 6, opening 3 units and reducing 1 unit
allocates 2 of the paid entry ticket plus 6 for the reduction to the closed
row. The remaining 2 units carry entry cost 4. Flattening those units costs
another ticket of 6: total paid across the three executions is 18.

`marked_equity(price)` is initial capital plus realized PnL and marked open
PnL, minus remaining paid entry costs of every fee type. At an unchanged mark,
a native execution decreases this value by its current charge, subject to
ordinary floating-point rounding. A rebate increases it.

## Integration and limits

The kernel currently serves full market exits and selected frozen-transaction
and same-side materialization paths. Existing source scheduling and dust
decisions remain at their call sites. Some source instructions still issue
separate close and open executions. Other legacy close loops are not yet
grouped into one parent execution; their per-row current ticket behavior is
not a claim about the native contract.

The shared close builder now consumes historical entry costs. This changes
the former reconstruction that converted both commission legs at exit-time
FX, and the former reuse of a complete entry ticket at every partial close.
TradingView compatibility must be measured against unchanged references.
Pine sizing and range-end reporting still have separate conventions; the
native marked-equity helper does not replace all existing report fields.

This API is synchronous. It does not place queued orders, establish matching
or cancellation authority, reconcile external broker messages, deduplicate
replayed fills or accept a stored plan as execution authority. Callers own
those responsibilities and the current event context. The return status
distinguishes application, no effect and validation failures. Allocation or
lifecycle exceptions abort the owning run; callers must discard that failed
run rather than retry a partially committed execution in place. Strong
rollback on allocation failure is not promised.

The work does not remove `ShortSeedCollisionRole` or complete migration to a
generic queued order machine. Existing executable state remains represented
in ABI projections and fingerprints.
