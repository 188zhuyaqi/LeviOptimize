#include "features/player_snapshot/PlayerSnapshotState.h"
#include <array>
#include <iostream>
#include <stdexcept>

using lo::player_snapshot_opt::detail::SnapshotState;
void require(bool value) {
    if (!value) throw std::runtime_error("snapshot state check failed");
}
int main() {
    SnapshotState state;
    std::array    first{1, 2, 3}, changed{1, 4, 3};
    auto          bytes = std::as_bytes(std::span{first});
    require(!state.matches(bytes));
    state.replace(bytes);
    require(state.matches(bytes));
    require(!state.matches(std::as_bytes(std::span{changed})));
    require(!state.matches(bytes.first(2)));
    {
        SnapshotState::Visit outer(state);
        require(!state.canRebuild());
        try {
            SnapshotState::Visit inner(state);
            throw 1;
        } catch (int) {}
        require(!state.canRebuild());
        require(state.matches(bytes));
    }
    require(state.canRebuild());
    state.retire();
    require(state.retired() && !state.matches(bytes) && !state.canRebuild());
    SnapshotState empty;
    empty.replace({});
    require(empty.matches({}));
    std::cout << "PASS snapshot identity, changed bytes, nested unwind, retirement, empty users\n";
}
