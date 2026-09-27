// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Placement plans: the artifact this library produces, and what it is not.
//
// A plan is a proposal. It records where an asset may go, in what order the
// candidate places rank, and exactly why every other candidate in the snapshot was
// rejected or could not be decided. It grants nothing. Producing a plan does not
// reserve a rack unit, does not consume a watt, does not tell any registry that
// anything happened, and does not prevent another plan - for this asset or any
// other - from naming the same location. Installing an asset is the job of the
// facility lifecycle authority; claiming capacity is the job of the reservation
// authority; deciding whether the claim is consistent with what actually happened
// is the job of the reconciliation authority. This library answers "where may it
// go, in what order, and why" and stops there.
//
// Every plan also records the exact facility state it was produced against, as a
// generation and a content fingerprint, and the semantics version it was produced
// under. Those three values are what make revalidation a computation rather than
// a judgement call.

#ifndef FACILITY_PLACEMENT_PLANNER_PLAN_HPP
#define FACILITY_PLACEMENT_PLANNER_PLAN_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/policy.hpp"
#include "facility_placement_planner/request.hpp"
#include "facility_placement_planner/requirements.hpp"
#include "facility_placement_planner/snapshot.hpp"
#include "facility_placement_planner/strong_types.hpp"
#include "facility_placement_planner/version.hpp"

namespace facility_placement_planner {

/// The admissibility rules, in the order they are evaluated.
///
/// The order is part of the contract, not an implementation detail. A candidate
/// that violates several rules is reported against the first one in this order,
/// so two implementations of this library that agree on the rules agree on which
/// reason a candidate is refused. The numeric values are stable and persisted.
enum class RuleId : std::uint16_t {
    CandidateStateOffered = 10,
    PolicyResolved = 20,
    RackTypeAllowed = 30,
    LocationAllowed = 40,
    SiteZoneAllowed = 50,
    RackTotalCeiling = 60,
    SpaceAvailable = 70,
    RackUnitsAvailable = 80,
    SlotsAvailable = 90,
    WeightAvailable = 100,
    PowerAvailable = 110,
    PowerRedundancySatisfied = 120,
    CoolingAvailable = 130,
    AirflowAvailable = 140,
    ServiceabilitySatisfied = 150,
    TenantIsolationSatisfied = 160,
    AssetCountCeiling = 170,
    DependencySatisfied = 180,
    AntiAffinitySatisfied = 190,
    FailureDomainPermitted = 200,
    UtilizationCeiling = 210,
    AssetNotAlreadyPlaced = 220,
};

/// Number of rules, for sizing the fixed trace array.
inline constexpr std::size_t kRuleCount = 22;

[[nodiscard]] FPP_API std::string_view to_string(RuleId rule) noexcept;
[[nodiscard]] FPP_API std::optional<RuleId> rule_id_from_token(std::string_view token) noexcept;

/// Why a candidate was refused, or why it could not be decided.
///
/// Codes 1..28 are proofs of inadmissibility. Codes 29..33 are statements that the
/// question could not be answered. A caller that treats the second group as the
/// first has turned "I do not know" into "no", and a caller that treats the first
/// as the second has turned a proof into a doubt. The two groups never mix inside
/// one verdict.
enum class RejectionCode : std::uint16_t {
    None = 0,

    // --- Proofs of inadmissibility -----------------------------------------
    CandidateWithdrawn = 1,
    CandidateQuarantined = 2,
    PolicyNotFound = 3,
    PolicyGenerationMismatch = 4,
    RackTypeNotAllowed = 5,
    LocationNotAllowed = 6,
    SiteNotAllowed = 7,
    ZoneNotAllowed = 8,
    RackTotalExceeded = 9,
    InsufficientSpace = 10,
    InsufficientRackUnits = 11,
    InsufficientSlots = 12,
    InsufficientWeightAllowance = 13,
    InsufficientPower = 14,
    PowerRedundancyUnsatisfied = 15,
    InsufficientCooling = 16,
    InsufficientAirflow = 17,
    ServiceabilityUnsatisfied = 18,
    TenantIsolationViolation = 19,
    AssetCountCeilingExceeded = 20,
    DependencyNotSatisfied = 21,
    DependencyGenerationMismatch = 22,
    DependencyAssetMissing = 23,
    AntiAffinityViolation = 24,
    ForbiddenFailureDomain = 25,
    UtilizationCeilingExceeded = 26,
    AssetAlreadyPlaced = 27,

    // --- Statements that the question could not be answered -----------------
    EvidenceSourceAbsent = 28,
    EvidenceUnknown = 29,
    EvidenceUnsupported = 30,
    EvidenceUnavailable = 31,
    EvidenceGenerationMismatch = 32,
    SelectionConstraintUnresolved = 33,
};

[[nodiscard]] FPP_API std::string_view to_string(RejectionCode code) noexcept;
[[nodiscard]] FPP_API std::optional<RejectionCode> rejection_code_from_token(std::string_view token) noexcept;

/// True when the code proves inadmissibility rather than reporting doubt.
[[nodiscard]] FPP_API bool is_definite_rejection(RejectionCode code) noexcept;

enum class RuleVerdict : std::uint8_t {
    Satisfied = 0,
    Violated = 1,
    Indeterminate = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(RuleVerdict verdict) noexcept;

/// What one rule decided about one candidate, with the exact numbers involved.
///
/// `observed` and `required` are the magnitudes the rule compared, in the rule's
/// own unit: milliwatts for the power rules, tile units for space, permille for
/// utilization ceilings, a small enumeration for the structural rules. They are
/// signed so that a rule can report a negative headroom without a separate flag,
/// and they are the reason an explanation can say "needs 4000000 mW, 3100000
/// available" rather than "insufficient power".
struct FPP_API RuleOutcomeRecord {
    RuleId rule = RuleId::CandidateStateOffered;
    RuleVerdict verdict = RuleVerdict::Satisfied;
    RejectionCode code = RejectionCode::None;
    std::int64_t observed = 0;
    std::int64_t required = 0;
};

enum class CandidateVerdict : std::uint8_t {
    Admissible = 0,
    Rejected = 1,
    Indeterminate = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(CandidateVerdict verdict) noexcept;

/// One preference key for one candidate.
struct FPP_API RankKey {
    PreferenceCriterion criterion = PreferenceCriterion::LowestLocationOrdinal;
    /// The measured magnitude, normalised so that a larger value is always
    /// better, whatever the criterion actually measures.
    std::int64_t raw = 0;
    std::int64_t weight = 0;
    /// `raw * weight`, saturated rather than wrapped when the product does not
    /// fit. Larger is better. The comparison over these keys is lexicographic in
    /// the request's preference order.
    std::int64_t key = 0;
};

/// The complete decision for one candidate.
struct FPP_API CandidateDecision {
    LocationId location;
    CandidateVerdict verdict = CandidateVerdict::Indeterminate;
    /// The first rule, in evaluation order, that produced this verdict. `None`
    /// only when the candidate is admissible.
    RejectionCode primary = RejectionCode::None;
    /// Every rule that did not pass, in evaluation order, not only the first.
    /// An explanation that named only the first refusal would hide the second one
    /// that the caller also has to fix.
    std::vector<RejectionCode> all_rejections;
    /// The full rule trace, in evaluation order, including the rules that passed.
    std::vector<RuleOutcomeRecord> trace;
    /// Rank keys, empty unless the candidate is admissible.
    std::vector<RankKey> keys;
    /// Position in the final deterministic order, among admissible candidates
    /// only. Zero-based; `kNotRanked` for candidates that are not admissible.
    std::uint64_t rank = 0;
    /// How many instances the selection search took from this candidate.
    InstanceCount selected_instances = InstanceCount{0};

    static constexpr std::uint64_t kNotRanked = UINT64_MAX;
};

/// What the planning run concluded about the request as a whole.
enum class PlanOutcome : std::uint8_t {
    /// At least one admissible selection was found and is recorded.
    Planned = 0,
    /// The search completed and proved that no admissible selection exists. This
    /// is a positive result: every candidate was examined, every rejection is
    /// definite, and no arrangement of them satisfies the constraints.
    Infeasible = 1,
    /// The search could not decide. Either mandatory evidence was missing or not
    /// fresh, or the search budget ran out before the answer was proved. No
    /// claim is made about whether a placement exists.
    Indeterminate = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(PlanOutcome outcome) noexcept;

/// Whether a plan may still be relied upon.
///
/// This is lifecycle, not content. A plan produced on a facility that has since
/// changed keeps its recorded content and outcome; what changes is whether the
/// facility it described is still the facility that exists.
enum class PlanValidity : std::uint8_t {
    /// Produced or revalidated against the snapshot the caller currently holds,
    /// and not expired.
    Valid = 0,
    /// Recovered from persistent state and not yet revalidated. This is the state
    /// every plan loaded from a store begins in. Persisted evidence does not
    /// become current evidence by being read back.
    RevalidationRequired = 1,
    /// A snapshot for the same facility was published at a later generation, so
    /// the plan describes a facility state that has been superseded. Re-planning
    /// against the newer snapshot is the only way to restore confidence.
    StaleSnapshot = 2,
    /// The plan's age bound has passed.
    Expired = 3,
    /// Explicitly revoked: a policy changed, the asset generation moved, an
    /// operator withdrew it, or the request was superseded.
    Invalidated = 4,
};

[[nodiscard]] FPP_API std::string_view to_string(PlanValidity validity) noexcept;

/// Why a plan was invalidated. Persisted, so numeric values are stable.
enum class InvalidationCause : std::uint8_t {
    ExplicitRevocation = 0,
    PolicyChanged = 1,
    AssetGenerationChanged = 2,
    SnapshotRebuilt = 3,
    TenantChanged = 4,
    OperatorWithdrawal = 5,
    TickExpired = 6,
    Superseded = 7,
};

[[nodiscard]] FPP_API std::string_view to_string(InvalidationCause cause) noexcept;
[[nodiscard]] FPP_API std::optional<InvalidationCause> invalidation_cause_from_token(
    std::string_view token) noexcept;

/// A durable record of one invalidation.
struct FPP_API InvalidationRecord {
    PlanId plan;
    PlanGeneration generation;
    InvalidationCause cause = InvalidationCause::ExplicitRevocation;
    Tick tick;
    std::string detail;
};

/// Deterministic search accounting. Every number here is a count of things this
/// build did, not a measurement of time.
struct FPP_API SearchStatistics {
    std::uint64_t candidates_in_snapshot = 0;
    std::uint64_t candidates_examined = 0;
    std::uint64_t admissible = 0;
    std::uint64_t rejected = 0;
    std::uint64_t indeterminate = 0;
    std::uint64_t selection_nodes_expanded = 0;
    std::uint64_t feasible_sets_found = 0;
    /// True when a bound stopped the run before it finished. An Indeterminate
    /// outcome with this flag set means "not enough budget"; without it, the
    /// indeterminacy came from evidence.
    bool budget_exhausted = false;
    SearchBudget budget;
};

/// One instance of the placement selection.
struct FPP_API SelectedPlacement {
    LocationId location;
    /// Which instance of the request this selection satisfies, zero-based in
    /// selection order.
    InstanceCount instance_index = InstanceCount{0};
    /// Position in the deterministic selection order.
    std::uint64_t sequence = 0;
};

/// The proposal artifact.
struct FPP_API PlacementPlan {
    PlanId id;
    /// Revision of this plan for this request. A re-plan against a newer snapshot
    /// is a new generation; a revalidation that changes nothing is not.
    PlanGeneration generation;

    RequestId request;
    AssetRef asset;
    TenantId tenant;

    /// The exact facility state this plan was produced against.
    SnapshotBinding snapshot;
    /// The planning semantics this plan was produced under. A plan carrying a
    /// different value is not re-read as if it meant the same thing.
    std::uint16_t semantics_version = 0;

    PlanOutcome outcome = PlanOutcome::Indeterminate;
    /// For `Infeasible`, the first rejection in evaluation order across the whole
    /// candidate set, which is the most informative single reason. `None`
    /// otherwise.
    RejectionCode terminal_rejection = RejectionCode::None;
    /// For `Indeterminate`, the operational category of the reason.
    ErrorCode indeterminate_reason = ErrorCode::None;
    /// Human-readable statement of the indeterminedness, empty otherwise.
    std::string indeterminate_detail;

    /// The evidence gate result the run applied, recorded so that a reader can
    /// see what the planner knew without re-deriving it.
    EvidenceGate evidence_gate;

    /// Every candidate in the snapshot that was examined, in candidate identity
    /// order. Candidates beyond the examination budget are absent, and their
    /// absence is what `stats.budget_exhausted` records.
    std::vector<CandidateDecision> decisions;

    /// The chosen selection, in selection order. Empty unless `Planned`.
    std::vector<SelectedPlacement> selection;

    SearchStatistics stats;

    Tick created_tick;
    /// Tick at which this plan stops being valid, or zero when it does not
    /// expire by age.
    Tick expires_at_tick;

    PlanValidity validity = PlanValidity::Valid;

    /// Recorded when the plan was invalidated. `ExplicitRevocation` and a zero
    /// tick mean the plan has never been invalidated.
    InvalidationCause invalidation_cause = InvalidationCause::ExplicitRevocation;
    Tick invalidated_at_tick;
    std::string invalidation_detail;

    [[nodiscard]] bool is_planned() const noexcept { return outcome == PlanOutcome::Planned; }

    /// The decision recorded for `location`, or nullptr. Valid while this plan is
    /// alive.
    [[nodiscard]] const CandidateDecision* find_decision(LocationId location) const noexcept;

    void absorb(FingerprintBuilder& builder) const noexcept;
};

/// A machine-readable, deterministic explanation of a plan.
///
/// Written for a caller that has to answer "why not that rack" without reading
/// the structs. The rendering depends only on the plan's contents: no addresses,
/// no clock, no locale, no iteration over an unordered container.
[[nodiscard]] FPP_API std::string explain_plan(const PlacementPlan& plan);

/// Plans held in memory, with the lifecycle rules that govern them.
///
/// The ledger is a store of proposals, not of authority. It enforces that a
/// request has at most one current plan generation, that a superseded plan is
/// neither current nor usable, and that a plan recovered from persistence starts
/// unusable until it is revalidated. It never mutates a plan's recorded outcome.
///
/// Concurrency: the ledger owns exactly one mutex and never calls out while
/// holding it. Readers receive a copy; there is no path that returns a reference
/// into the ledger's storage, so a reader can never observe a plan being replaced.
class FPP_API PlanLedger {
public:
    explicit PlanLedger(PlannerLimits limits = {});

    /// Movable, not copyable. The special members are defined out of line because
    /// the implementation type is incomplete here, and a `unique_ptr` member would
    /// otherwise be deleted at a point where it has no destructor.
    PlanLedger(PlanLedger&& other) noexcept;
    PlanLedger& operator=(PlanLedger&& other) noexcept;
    PlanLedger(const PlanLedger&) = delete;
    PlanLedger& operator=(const PlanLedger&) = delete;
    ~PlanLedger();

    /// Records a newly produced plan. Refuses a duplicate (request, generation),
    /// and refuses a generation that is not newer than the request's current one.
    Outcome<PlacementPlan> record(PlacementPlan plan);

    /// The plan with `id`, or NotFound.
    [[nodiscard]] Outcome<PlacementPlan> find(PlanId id) const;

    /// The current plan for `request`, or NotFound.
    [[nodiscard]] Outcome<PlacementPlan> current_for_request(RequestId request) const;

    /// Every plan, ordered by (request, generation).
    [[nodiscard]] std::vector<PlacementPlan> all() const;

    /// Marks a plan invalid and returns the updated copy. Refuses an already
    /// invalid plan with PreconditionFailed: invalidation is not idempotent at
    /// this level, because a second invalidation under a different cause would
    /// silently rewrite the recorded reason.
    Outcome<PlacementPlan> invalidate(PlanId id, InvalidationCause cause, Tick tick, std::string detail);

    /// Recomputes validity from age alone. Returns the recomputed value and
    /// stores it.
    Outcome<PlanValidity> evaluate_validity(PlanId id, Tick now);

    /// Replaces a plan's validity with the result of a revalidation, without
    /// touching its recorded outcome, snapshot binding, or selection.
    Outcome<PlacementPlan> apply_revalidation(PlanId id, PlanValidity validity);

    [[nodiscard]] std::size_t size() const;

    /// The bounds this ledger applies. Defined out of line because the
    /// implementation type is incomplete here.
    [[nodiscard]] const PlannerLimits& limits() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_PLAN_HPP
