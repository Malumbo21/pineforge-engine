# Broker state and fill ownership

The broker processes a source-ordered command book against a price path. Its
physical position, logical close claims, reserved quantities and pyramiding
capacity are separate ledgers. A Pine entry ID is a reusable name; the order
incarnation and position cycle identify the owners of those ledgers.

This document describes the first extracted ownership model. Other order,
reservation and path state is still represented in `BacktestEngine` and
`PendingOrder`; their consolidation is ongoing. The current economic rules
retain their tested domains. Moving a rule into a type does not establish
that it describes every TradingView configuration.

## What qualifies as a generic flag

A retained flag must represent a defined user setting or a causal execution
fact with an owner, a producer and an expiry rule. Examples are a requested
execution mode or a stop leg that actually activated. Multiple overlapping
facts describing one lifecycle need explicit states and transitions instead.
An experimental interpretation switch is not a user setting merely because
it can be passed through metadata.

Review both the fact and its consumers. A generic fact such as "market fill
at the open" does not justify every financial exception that reads it. Each
consumer must have an economic, temporal or ownership explanation and a
counterfactual that can refute it independently of the originating strategy.
Changing the strategy's name, adding an unreachable order, splitting physical
lots or varying price scale can reveal a condition that encodes a narrow
example rather than the proposed rule; the comparison must hold the relevant
economics and information constant.

In particular, a conjunction of direction, commission, sizing and book shape
does not become generic by being renamed as an enum or captured as a receipt.
The opening extraction below fixes ownership; the surviving financial
eligibility predicates still require that separate review. Existing parity
scores are regression evidence, not a justification for keeping a compensating
flag. Actual conflicting TradingView observations belong in an anomaly review
record, separate from an unknown rule or an engine defect.

## Opening checkpoint

An accepted opening or add can create an opening-affordability checkpoint.
`broker::OpeningReceipt` carries its producing broker fill, source order
incarnation, position cycle, bar and timestamp, and the raw matched price.
The booked entry price is separate: the financial check reads the current
position book and uses the raw price only where the execution policy requires
it. It does not reconstruct ownership from FIFO rows or entry names.

The live states are absent, pending check, and pending exemption. A check can
carry a remaining-adverse-path continuation; an exemption cannot. The
decision is made by the successful-fill policy when it creates the receipt.
There is no later transition that promotes an exemption into eligibility.

| Operation | Transition |
|---|---|
| Qualifying successful opening/add | Replace with the new producer's check or exemption |
| Rejected or zero-effect attempt | Preserve the pending receipt |
| Accepted incompatible short opening/add | Invalidate the prior receipt |
| Full close, reversal, fresh cycle, run reset | Invalidate prior-position ownership |
| Ordinary margin checkpoint | Take the receipt before any early return or recursive check; reject a stale position-cycle owner |
| Checkpoint before a priced exit | Inspect without consuming; after an actual margin slice, consume only the same producer's receipt |

This is one coalescing checkpoint over the aggregate book. It is not a queue
that retroactively settles every individual fill. A later qualifying add
replaces the raw price and producer; a no-op cannot replace them. A partial
FIFO close may leave the position cycle alive even if it drains the original
physical lot, so the receipt owns the aggregate cycle rather than requiring
that original lot to survive.

Disabled margin, invalid financial data, exemption and no shortfall can all
consume the ordinary checkpoint without generating a trade. A scoped short
check may then visit the remaining adverse path once using its detached
receipt. The pre-priced-exit consumer retains its different existing contract:
a no-action inspection leaves the receipt pending for the ordinary check.

Four historical long/short lifecycle labels had no economic consumers. They
have been removed, along with their producers; the numerical floor-zero rules
and their trade fixtures remain. Those labels are not alternate model states.

## Distinct execution domains

An opening receipt is broker-local. It is not an identity for every output:

- A physical fill may produce several FIFO trade rows.
- A COOF notification may group several resting-order fills.
- A stream action is an observer projection with its own delivery sequence.
- A range-end report row marks an open position for reporting; it does not
  close the broker book or create a stream action.
- Pine variables and source-series history have separate speculative and
  committed states. A Pine rollback does not roll back committed broker fills.

Historical OHLC, magnifier, confirmed-bar and actual-tick inputs expose
different information. The opening ownership extraction changes none of their
path, callback or warmup policies. Terminal-close deferral is still selected
by the existing financial policy; it has not been generalized by this model.

## Verification and compatibility

Literal state tests exercise consume-before-use, exempt presence, replacement,
stale-owner rejection, no-op preservation and independent copies. Existing
trade fixtures retain their financial expectations. Cloud comparison of the
fixed reference population is required before reporting parity preservation.
Hashes supplement those comparisons; a matching fingerprint is not a proof
that all hidden strategy state is equal.

The new receipt replaces protected C++ members and changes the fingerprint
representation. Rebuild generated and native modules against matching headers
and runtime. The cap extraction advances the internal class namespace to
`engine_script_run_v3`, broker hash domain to `pineforge-broker-state/v3`, and
stream fingerprint prefix to 3. These pairing/serialization versions change
no financial rule; public C signatures, POD layouts and API versions remain
unchanged. See
[ABI stability](abi-stability.md).
