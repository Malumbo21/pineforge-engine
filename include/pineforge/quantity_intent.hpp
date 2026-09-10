#pragma once

#include <cmath>
#include <optional>
#include <stdexcept>
#include <variant>

namespace pineforge {

enum class QuantityIntentKind { Units, Fraction, All };

// A requested amount, before executable quantity is rounded, reserved or
// reduced. A fraction retains its numerator and denominator so a frontend can
// preserve the caller's exact representation (for example 50 / 100).
class QuantityIntent {
    struct Units { double amount; };
    struct Fraction { double numerator; double denominator; };
    struct All {};
    using Value = std::variant<Units, Fraction, All>;
public:
    using Kind = QuantityIntentKind;
    static QuantityIntent units(double amount) { return QuantityIntent(Units{amount}); }
    static QuantityIntent fraction(double numerator, double denominator) {
        if (!std::isfinite(denominator) || denominator <= 0.0)
            throw std::invalid_argument("quantity fraction requires a positive finite denominator");
        return QuantityIntent(Fraction{numerator, denominator});
    }
    static QuantityIntent all() { return QuantityIntent(All{}); }
    Kind kind() const { return static_cast<Kind>(value_.index()); }
    double units() const { return std::get<Units>(value_).amount; }
    double numerator() const { return std::get<Fraction>(value_).numerator; }
    double denominator() const { return std::get<Fraction>(value_).denominator; }
private:
    explicit QuantityIntent(Value value) : value_(value) {}
    Value value_;
};

// A reservation is a causal quantity snapshot. Later OCA reductions of the
// executable order do not rewrite its original request or this basis.
struct QuantityReservation {
    double units;
    double basis_units;
};

class QuantityRequest {
public:
    const std::optional<QuantityIntent>& intent() const { return intent_; }
    const std::optional<QuantityReservation>& reservation() const { return reservation_; }
    void request(QuantityIntent intent) {
        intent_ = intent;
        reservation_.reset();
    }
    void reserve(double units, double basis_units) {
        if (!intent_) throw std::logic_error("quantity reservation requires an original request");
        reservation_ = QuantityReservation{units, basis_units};
    }
    bool requests_all() const {
        return intent_ && intent_->kind() == QuantityIntent::Kind::All;
    }
    // Tolerances belong to the caller's quantity policy. No minimum lot,
    // percentage rounding or strategy-specific eligibility lives here.
    bool is_partial(double units_tolerance, double fraction_tolerance) const {
        if (reservation_)
            return reservation_->units < reservation_->basis_units - units_tolerance;
        return intent_ && intent_->kind() == QuantityIntent::Kind::Fraction
            && intent_->numerator() < intent_->denominator() - fraction_tolerance;
    }
private:
    // Absent for commands that do not carry this exit-request contract.
    std::optional<QuantityIntent> intent_;
    std::optional<QuantityReservation> reservation_;
};

} // namespace pineforge
