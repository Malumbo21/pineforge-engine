// Generic order cancellation receipt: causal identity and once-only claim
// release are native broker state, independent of a Pine compatibility flag.
#include <pineforge/order_cancellation.hpp>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace pineforge;
namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
}

int main() {
    OrderCancellationReceipt receipt;
    CHECK(!receipt.cancelled());
    CHECK(receipt.cause() == CancellationCause::None);
    receipt.bind_close_claim(3.0, 0.5);
    CHECK(receipt.close_claim_release() == CloseClaimRelease::Pending);
    CHECK(!receipt.cancel(CancellationCause::Dependency, 11, 0, 12));
    CHECK(!receipt.cancel(CancellationCause::Dependency, 0, 4, 12));
    CHECK(!receipt.cancel(CancellationCause::Dependency, 11, 4, 12, -1, 3));
    CHECK(!receipt.cancel(CancellationCause::Dependency, 11, 4, 12, 7,
                         std::numeric_limits<uint64_t>::max()));
    CHECK(receipt.cancel(CancellationCause::Dependency, 11, 4, 12, 7, 3));
    CHECK(receipt.cancelled());
    CHECK(receipt.source_incarnation() == 11);
    CHECK(receipt.source_sequence() == 4);
    CHECK(receipt.target_incarnation() == 12);
    CHECK(receipt.target_owner() == 7);
    CHECK(receipt.target_revision() == 3);
    double ledger = 7.0;
    CHECK(receipt.release_close_claim_once(ledger));
    CHECK(std::abs(ledger - 10.5) < 1e-12);
    CHECK(receipt.close_claim_release() == CloseClaimRelease::Released);
    CHECK(!receipt.release_close_claim_once(ledger));
    CHECK(std::abs(ledger - 10.5) < 1e-12);
    CHECK(!receipt.bind_close_claim(8.0, 0.0));
    CHECK(receipt.close_claim_release() == CloseClaimRelease::Released);

    OrderCancellationReceipt replacement;
    replacement.bind_close_claim(std::numeric_limits<double>::quiet_NaN(), 0.0);
    CHECK(replacement.cancel(CancellationCause::Replacement, 9, 3, 10));
    CHECK(replacement.close_claim_release() == CloseClaimRelease::NotApplicable);
    CHECK(!replacement.release_close_claim_once(ledger));
    std::printf("order_cancellation: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
