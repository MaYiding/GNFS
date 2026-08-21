#pragma once

/// @file raw_relation_corpus_view.hpp
/// @brief Non-owning, allocation-free view over two raw SIQS relation segments.

#include <gnfs/siqs/relation.hpp>

#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <span>

namespace gnfs::siqs {

using std::size_t;

/// Read-only logical concatenation of a legacy raw corpus and one supplemental
/// shadow corpus.
///
/// The two backing spans and every relation they expose must remain alive,
/// address-stable, and unmodified for the lifetime of the view and its
/// iterators. The view object itself must also remain alive at the same address
/// while any iterator obtained from it is used. Iteration visits the first span
/// followed by the second without allocating or copying relations. Overlapping
/// spans are permitted and are interpreted as logical duplicate input.
class SIQSRawRelationCorpusView final {
public:
    using segment_type = std::span<const SIQSRelation>;

    class const_iterator final {
    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        using value_type = SIQSRelation;
        using difference_type = std::ptrdiff_t;
        using pointer = const SIQSRelation*;
        using reference = const SIQSRelation&;

        constexpr const_iterator() noexcept = default;

        [[nodiscard]] constexpr reference operator*() const noexcept {
            return owner_->relation_at_unchecked(ordinal_);
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept {
            return &operator*();
        }

        constexpr const_iterator& operator++() noexcept {
            ++ordinal_;
            return *this;
        }

        constexpr const_iterator operator++(int) noexcept {
            const_iterator previous = *this;
            ++*this;
            return previous;
        }

        [[nodiscard]] friend constexpr bool operator==(const const_iterator&,
                                                       const const_iterator&) noexcept = default;

    private:
        friend class SIQSRawRelationCorpusView;

        constexpr const_iterator(const SIQSRawRelationCorpusView* owner, size_t ordinal) noexcept
            : owner_(owner), ordinal_(ordinal) {}

        const SIQSRawRelationCorpusView* owner_ = nullptr;
        size_t ordinal_ = 0;
    };

    /// Construct a source-compatible one-segment view. A single span's size is
    /// already representable in size_t, so no checked factory is needed.
    explicit constexpr SIQSRawRelationCorpusView(segment_type contiguous) noexcept
        : segments_{contiguous, segment_type{}}, size_(contiguous.size()) {}

    /// Construct a two-segment view after checking the logical relation count.
    [[nodiscard]] static constexpr std::optional<SIQSRawRelationCorpusView>
    try_create(segment_type first, segment_type second) noexcept {
        if (second.size() > std::numeric_limits<size_t>::max() - first.size()) {
            return std::nullopt;
        }
        return SIQSRawRelationCorpusView(first, second, first.size() + second.size(),
                                         UncheckedTag{});
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr const_iterator begin() const noexcept {
        return const_iterator(this, 0);
    }

    [[nodiscard]] constexpr const_iterator end() const noexcept {
        return const_iterator(this, size_);
    }

private:
    struct UncheckedTag final {};

    constexpr SIQSRawRelationCorpusView(segment_type first, segment_type second, size_t size,
                                        UncheckedTag) noexcept
        : segments_{first, second}, size_(size) {}

    [[nodiscard]] constexpr const SIQSRelation&
    relation_at_unchecked(size_t ordinal) const noexcept {
        if (ordinal < segments_[0].size()) {
            return segments_[0][ordinal];
        }
        return segments_[1][ordinal - segments_[0].size()];
    }

    std::array<segment_type, 2> segments_;
    size_t size_;
};

} // namespace gnfs::siqs
