// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/clipboardhistoryguard.h"

#include <cstdlib>
#include <iostream>

namespace {

using snow_shot::presentation::ClipboardHistoryGuard;
using Decision = ClipboardHistoryGuard::Decision;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void externalCopy(ClipboardHistoryGuard& guard, std::uint32_t sequence, std::int64_t at) {
    guard.noteUpdate(sequence, at);
    require(guard.evaluate(sequence, at) == Decision::Pending,
            "an external copy must wait for the clipboard to settle");
    require(guard.evaluate(sequence, at + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::Record,
            "an external copy must become a history entry");
}

void ownRepaste(ClipboardHistoryGuard& guard, std::uint32_t sequence, std::int64_t at) {
    guard.markOwnWrite(sequence, at);
    guard.noteUpdate(sequence, at);
    require(guard.evaluate(sequence, at + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::SkipOwnWrite,
            "a re-paste must never create a history entry");
}

} // namespace

int main() {
    ClipboardHistoryGuard guard;

    // Plain external copies are recorded.
    externalCopy(guard, 101, 1000);
    externalCopy(guard, 102, 2000);

    // Re-pasting history items is filtered out.
    ownRepaste(guard, 103, 3000);
    ownRepaste(guard, 104, 4000);

    // Interleaving must not lose real user copies.
    externalCopy(guard, 105, 5000);
    ownRepaste(guard, 106, 6000);
    externalCopy(guard, 107, 7000);
    ownRepaste(guard, 108, 8000);
    require(guard.handledSequence() == 108, "the guard must track the latest handled sequence");

    // A second notification for the same sequence is a duplicate.
    guard.noteUpdate(108, 9000);
    require(guard.evaluate(108, 9000 + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::SkipDuplicate,
            "duplicate notifications must not add another entry");

    // A stale own-write marker must not swallow a later real copy.
    guard.markOwnWrite(200, 10000);
    const std::int64_t expired = 10000 + ClipboardHistoryGuard::kOwnWriteTtlMs + 1;
    guard.noteUpdate(201, expired);
    require(guard.evaluate(201, expired + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::Record,
            "an expired own-write marker must not suppress a real copy");

    // A publisher can split one copy into several writes; the guard follows
    // the newest sequence instead of recording an intermediate state.
    guard.noteUpdate(300, 20000);
    require(guard.evaluate(301, 20000 + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::Pending,
            "a sequence that moved during settling must keep waiting");
    guard.noteUpdate(301, 20100);
    require(guard.evaluate(301, 20100 + ClipboardHistoryGuard::kSettleDelayMs) ==
                Decision::Record,
            "the final settled sequence must be recorded once");

    // The settle limit prevents an endlessly changing clipboard from stalling.
    guard.noteUpdate(400, 30000);
    require(guard.evaluate(400, 30000 + ClipboardHistoryGuard::kSettleLimitMs + 1) ==
                Decision::Record,
            "the settle limit must bound the wait");

    // reset() returns the guard to its initial state.
    guard.reset();
    require(!guard.hasPending() && guard.handledSequence() == 0,
            "reset must clear all guard state");
    externalCopy(guard, 1, 1);

    return EXIT_SUCCESS;
}
