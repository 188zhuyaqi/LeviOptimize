#pragma once
#include <algorithm>
#include <vector>

namespace lo::features::detail {

// A batch belongs to one call stack on one thread. Successful nested calls merge
// into their caller; unwinding discards that scope's pointers and restores it.
template <class Policy>
class InfluenceBatch {
    inline static thread_local InfluenceBatch* mActive{};

    InfluenceBatch*      mPrevious = mActive;
    std::vector<Policy*> mPending;
    bool                 mFinished{};

public:
    InfluenceBatch() { mActive = this; }
    ~InfluenceBatch() { mActive = mPrevious; }

    InfluenceBatch(InfluenceBatch const&)            = delete;
    InfluenceBatch& operator=(InfluenceBatch const&) = delete;

    static bool defer(Policy* policy) {
        if (!mActive) return false;
        auto& updates = mActive->mPending;
        if (!std::ranges::contains(updates, policy)) {
            updates.push_back(policy);
        }
        return true;
    }

    template <class Update>
    void finish(Update&& update) {
        if (mFinished) return;
        mFinished = true;
        mActive   = mPrevious;
        if (mPrevious) {
            for (auto* policy : mPending) defer(policy);
        } else {
            for (auto* policy : mPending) update(policy);
        }
    }
};

} // namespace lo::features::detail
