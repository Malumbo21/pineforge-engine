# Frozen public mirror prefix

`ff54_pending_order_mirror.hpp` is copied byte-for-byte from engine
ff54a557ac751244dafd60df0bb22886ec35792d. `ff54_fields.inc` enumerates its
142 fields for compile-time type, size and offset assertions. The native
frozen instruction extends the public snapshot only after this full prefix.
This fixture contains no strategy, tape or expected trades.
