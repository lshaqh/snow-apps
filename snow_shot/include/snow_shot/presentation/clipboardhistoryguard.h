// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYGUARD_H
#define SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYGUARD_H

#include <cstdint>
#include <optional>

namespace snow_shot::presentation {

// Decides whether a clipboard change notification becomes a history entry.
//
// Re-pasting a history item publishes to the clipboard too, which would
// otherwise grow the history by one entry per paste. The caller records the
// clipboard sequence number right after its own write; a notification that
// settles on that same sequence is discarded. A settle delay is required
// because publishers often split one copy into several OpenClipboard calls.
//
// This state machine is platform independent: the caller supplies the sequence
// number and a monotonic millisecond clock, so it can be tested offscreen
// without a real clipboard.
class ClipboardHistoryGuard final {
  public:
    static constexpr std::int64_t kSettleDelayMs = 100;
    static constexpr std::int64_t kSettleLimitMs = 2000;
    static constexpr std::int64_t kOwnWriteTtlMs = 3000;

    enum class Decision {
        Pending,
        Record,
        SkipOwnWrite,
        SkipDuplicate,
    };

    // Call AFTER this process wrote a history item back to the clipboard.
    // `sequence` must be read after the write has completed.
    void markOwnWrite(std::uint32_t sequence, std::int64_t nowMs) {
        ownWriteSequence_ = sequence;
        ownWriteDeadlineMs_ = nowMs + kOwnWriteTtlMs;
    }

    // Call on every clipboard change notification.
    void noteUpdate(std::uint32_t sequence, std::int64_t nowMs) {
        if (!pendingSequence_ || *pendingSequence_ != sequence) {
            pendingSequence_ = sequence;
            firstChangeMs_ = nowMs;
            lastChangeMs_ = nowMs;
        }
    }

    // Drive from a timer until the result is no longer Pending. `sequence` is
    // the CURRENT clipboard sequence number, which may have moved again while
    // the guard was waiting for the clipboard to settle.
    [[nodiscard]] Decision evaluate(std::uint32_t sequence, std::int64_t nowMs) {
        if (!pendingSequence_) {
            return Decision::Pending;
        }
        if (nowMs - lastChangeMs_ < kSettleDelayMs &&
            nowMs - firstChangeMs_ < kSettleLimitMs) {
            return Decision::Pending;
        }
        if (sequence != *pendingSequence_) {
            // Another write arrived; keep waiting on the newest sequence.
            pendingSequence_ = sequence;
            lastChangeMs_ = nowMs;
            return Decision::Pending;
        }

        const std::uint32_t settled = sequence;
        pendingSequence_.reset();
        if (handledSequence_ != 0 && handledSequence_ == settled) {
            return Decision::SkipDuplicate;
        }
        handledSequence_ = settled;

        if (ownWriteSequence_ && *ownWriteSequence_ == settled &&
            nowMs < ownWriteDeadlineMs_) {
            ownWriteSequence_.reset();
            return Decision::SkipOwnWrite;
        }
        // A stale marker must never swallow a later real user copy.
        ownWriteSequence_.reset();
        return Decision::Record;
    }

    void reset() {
        pendingSequence_.reset();
        handledSequence_ = 0;
        ownWriteSequence_.reset();
        ownWriteDeadlineMs_ = 0;
        firstChangeMs_ = 0;
        lastChangeMs_ = 0;
    }

    [[nodiscard]] bool hasPending() const { return pendingSequence_.has_value(); }
    [[nodiscard]] std::uint32_t handledSequence() const { return handledSequence_; }

  private:
    std::optional<std::uint32_t> pendingSequence_;
    std::uint32_t handledSequence_ = 0;
    std::optional<std::uint32_t> ownWriteSequence_;
    std::int64_t ownWriteDeadlineMs_ = 0;
    std::int64_t firstChangeMs_ = 0;
    std::int64_t lastChangeMs_ = 0;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYGUARD_H
