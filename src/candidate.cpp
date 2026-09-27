// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/candidate.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 3> kStateTokens{{"offered", "withdrawn", "quarantined"}};

/// The more ignorant of two states. Used when a derived quantity depends on two
/// measurements and only one of them is missing: the result is missing in the
/// same way, and "we do not know" is reported ahead of "this facility does not
/// express it" so that the reason a candidate is undecidable is the most
/// fundamental one available.
[[nodiscard]] MeasureState combine(MeasureState lhs, MeasureState rhs) noexcept {
    if (lhs == MeasureState::Known) {
        return rhs;
    }
    if (rhs == MeasureState::Known) {
        return lhs;
    }
    return static_cast<std::uint8_t>(lhs) < static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

template <typename T>
[[nodiscard]] Measure<T> remainder(const Measure<T>& total, const Measure<T>& used) noexcept {
    const MeasureState state = combine(total.state(), used.state());
    if (state == MeasureState::Known) {
        const typename T::rep_type total_value = total.value().value();
        const typename T::rep_type used_value = used.value().value();
        // A recording whose usage exceeds its total yields no headroom. The
        // alternative reading - wrapping, or trusting the larger number - would
        // manufacture capacity out of an inconsistent record.
        return Measure<T>::known(T(total_value > used_value ? total_value - used_value : typename T::rep_type{0}));
    }
    switch (state) {
        case MeasureState::Unsupported:
            return Measure<T>::unsupported();
        case MeasureState::Unavailable:
            return Measure<T>::unavailable();
        case MeasureState::Unknown:
        case MeasureState::Known:
            break;
    }
    return Measure<T>::unknown();
}

void absorb_failure_domains(FingerprintBuilder& builder, const std::vector<FailureDomainId>& domains) noexcept {
    builder.update_u64(static_cast<std::uint64_t>(domains.size()));
    for (const FailureDomainId domain : domains) {
        detail::absorb(builder, domain);
    }
}

}  // namespace

std::string_view to_string(CandidateState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    if (index >= kStateTokens.size()) {
        return "unknown";
    }
    return kStateTokens[index];
}

std::optional<CandidateState> candidate_state_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kStateTokens.size(); ++index) {
        if (kStateTokens[index] == token) {
            return static_cast<CandidateState>(index);
        }
    }
    return std::nullopt;
}

Measure<TileUnits> CandidateLocation::remaining_space() const noexcept {
    return remainder(space.total, space.used);
}

Measure<RackUnits> CandidateLocation::remaining_rack_units() const noexcept {
    return remainder(rack.total_units, rack.used_units);
}

Measure<SlotCount> CandidateLocation::remaining_slots() const noexcept {
    return remainder(rack.total_slots, rack.used_slots);
}

Measure<PowerMilliwatts> CandidateLocation::remaining_power() const noexcept {
    return remainder(power.capacity, power.committed);
}

Measure<ThermalMilliwatts> CandidateLocation::remaining_cooling() const noexcept {
    return remainder(cooling.capacity, cooling.committed);
}

Measure<MassGrams> CandidateLocation::remaining_weight() const noexcept {
    return remainder(weight.capacity, weight.used);
}

bool CandidateLocation::has_failure_domain(FailureDomainId domain) const noexcept {
    return std::binary_search(failure_domains.begin(), failure_domains.end(), domain);
}

bool CandidateLocation::is_unoccupied() const noexcept { return occupant_tenant.value() == 0; }

Status CandidateLocation::validate(const PlannerLimits& limits) const {
    if (id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a candidate has the zero location identity");
    }
    if (rack_type.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a candidate has the zero rack type");
    }
    Status count_check =
        check_bound("max_failure_domains_per_candidate", failure_domains.size(),
                    limits.max_failure_domains_per_candidate);
    if (!count_check) {
        return count_check;
    }
    for (std::size_t index = 0; index < failure_domains.size(); ++index) {
        if (failure_domains[index].value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "a candidate lists the zero failure domain");
        }
        if (index > 0 && !(failure_domains[index - 1] < failure_domains[index])) {
            return make_error(ErrorCode::DuplicateIdentity,
                              "a candidate's failure domains are not strictly ascending and unique");
        }
    }
    return Status();
}

void CandidateLocation::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "candidate");
    detail::absorb(builder, id);
    detail::absorb(builder, site_id);
    detail::absorb(builder, zone_id);
    detail::absorb(builder, rack_id);
    detail::absorb(builder, slot_id);
    detail::absorb(builder, rack_type);
    builder.update_byte(static_cast<std::uint8_t>(state));

    detail::absorb_section(builder, "candidate.space");
    detail::absorb(builder, space.total);
    detail::absorb(builder, space.used);

    detail::absorb_section(builder, "candidate.rack_id");
    detail::absorb(builder, rack.total_units);
    detail::absorb(builder, rack.used_units);
    detail::absorb(builder, rack.total_slots);
    detail::absorb(builder, rack.used_slots);
    detail::absorb(builder, rack.asset_count);

    detail::absorb_section(builder, "candidate.power");
    detail::absorb(builder, power.capacity);
    detail::absorb(builder, power.committed);
    detail::absorb(builder, power.redundancy);

    detail::absorb_section(builder, "candidate.cooling");
    detail::absorb(builder, cooling.capacity);
    detail::absorb(builder, cooling.committed);
    detail::absorb(builder, cooling.airflow);

    detail::absorb_section(builder, "candidate.weight");
    detail::absorb(builder, weight.capacity);
    detail::absorb(builder, weight.used);

    detail::absorb_section(builder, "candidate.serviceability");
    detail::absorb(builder, serviceability.aisle);
    detail::absorb(builder, serviceability.access);

    detail::absorb_section(builder, "candidate.failure_domains");
    absorb_failure_domains(builder, failure_domains);

    detail::absorb_section(builder, "candidate.occupancy");
    detail::absorb(builder, occupant_tenant);
    detail::absorb(builder, same_tenant_instances);
    detail::absorb(builder, observed_tick);
}

std::string to_string(const CandidateLocation& candidate) {
    std::ostringstream out;
    out << "location " << candidate.id.value() << " site=" << candidate.site_id.value()
        << " zone=" << candidate.zone_id.value() << " rack=" << candidate.rack_id.value()
        << " slot=" << candidate.slot_id.value() << " rack_type=" << candidate.rack_type.value()
        << " state=" << to_string(candidate.state) << " domains=";
    for (std::size_t index = 0; index < candidate.failure_domains.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << candidate.failure_domains[index].value();
    }
    return out.str();
}

void PlacedAsset::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "placed_asset");
    detail::absorb(builder, asset.id);
    detail::absorb(builder, asset.generation);
    detail::absorb(builder, location);
    detail::absorb(builder, tenant);
    detail::absorb(builder, instances);
    detail::absorb(builder, placed_tick);
}

}  // namespace facility_placement_planner
