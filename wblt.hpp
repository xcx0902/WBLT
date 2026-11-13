#ifndef WBLT_HPP
#define WBLT_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

// A pool of move-only sequences. The pool must outlive its sequences.
// composition(f, g) applies g first. id is accepted for ACL compatibility;
// an absent lazy tag needs neither an identity action nor an F comparison.
template <class S, auto op, auto e, class F, auto mapping, auto composition,
          auto id = nullptr>
class WBLT {
    using Index = std::uint32_t;
    static constexpr Index leaf_bit = Index{1} << 31;
    static constexpr Index max_slots = leaf_bit - 1;

public:
    using size_type = std::size_t;

    class Sequence {
        friend class WBLT;
        WBLT* pool_ = nullptr;
        Index root_ = 0;

        Sequence(WBLT* pool, Index root) noexcept : pool_(pool), root_(root) {}

    public:
        Sequence() noexcept = default;
        Sequence(const Sequence&) = delete;
        Sequence& operator=(const Sequence&) = delete;

        Sequence(Sequence&& other) noexcept
            : pool_(other.pool_), root_(std::exchange(other.root_, 0)) {}

        Sequence& operator=(Sequence&& other) noexcept {
            if (this != &other) {
                if (root_) pool_->destroy(root_);
                pool_ = other.pool_;
                root_ = std::exchange(other.root_, 0);
            }
            return *this;
        }

        ~Sequence() {
            if (root_) pool_->destroy(root_);
        }

        bool empty() const noexcept { return root_ == 0; }
        size_type size() const noexcept { return root_ ? pool_->weight(root_) : 0; }
        S prod() const { return root_ ? pool_->product(root_) : e(); }

        std::vector<S> to_vector() const {
            std::vector<S> out;
            if (root_) {
                out.reserve(size());
                pool_->append_elements(root_, nullptr, out);
            }
            return out;
        }

        void apply(const F& f) {
            if (root_) pool_->apply_to(root_, f);
        }
    };

    WBLT() = default;
    explicit WBLT(size_type capacity) { reserve(capacity); }
    WBLT(const WBLT&) = delete;
    WBLT& operator=(const WBLT&) = delete;
    WBLT(WBLT&&) = delete;
    WBLT& operator=(WBLT&&) = delete;

    // Reserve storage for this many live elements across all sequences.
    void reserve(size_type capacity) {
        if (capacity > max_slots) throw std::length_error("WBLT capacity");
        leaves_.reserve(capacity);
        branches_.reserve(capacity ? capacity - 1 : 0);
    }

    Sequence singleton(const S& value) {
        return Sequence(this, make_leaf(value));
    }

    Sequence singleton(S&& value) {
        return Sequence(this, make_leaf(std::move(value)));
    }

    Sequence build(const std::vector<S>& values) {
        return build(values.begin(), values.end());
    }

    Sequence build(std::vector<S>&& values) {
        return build(std::make_move_iterator(values.begin()),
                     std::make_move_iterator(values.end()));
    }

    template <class RandomIt>
    Sequence build(RandomIt first, RandomIt last) {
        if (first == last) return {};
        const auto n = last - first;
        if (n < 0 || static_cast<size_type>(n) > max_slots)
            throw std::length_error("WBLT input size");
        return Sequence(this, build_nodes(first, static_cast<Index>(n)));
    }

    // Consumes the inputs, leaving their handles empty. Both nonempty inputs
    // must have been built by this pool.
    Sequence merge(Sequence&& left, Sequence&& right) {
        check_owner(left);
        check_owner(right);
        if (left.empty()) return std::move(right);
        if (right.empty()) return std::move(left);
        if (&left == &right) throw std::invalid_argument("WBLT self merge");
        const Index result = join(left.root_, right.root_);
        left.root_ = right.root_ = 0;
        return Sequence(this, result);
    }

    // The first result contains exactly k elements.
    std::pair<Sequence, Sequence> split(Sequence&& seq, size_type k) {
        check_owner(seq);
        if (k > seq.size()) throw std::out_of_range("WBLT split position");
        if (k == 0) return {Sequence{}, std::move(seq)};
        if (k == seq.size()) return {std::move(seq), Sequence{}};
        return take_split(seq, split_at(seq.root_, static_cast<Index>(k)));
    }

    // pred(element) must be false on a prefix and true on the rest.
    template <class Pred>
    std::pair<Sequence, Sequence> split_by(Sequence&& seq, Pred&& pred) {
        check_owner(seq);
        if (seq.empty() || !pred(last_element(seq.root_)))
            return {std::move(seq), Sequence{}};
        auto goes_left = [&](Index left) { return bool(pred(last_element(left))); };
        return take_split(seq, split_before_true(seq.root_, goes_left));
    }

    // pred(product of an inclusive prefix) must go from false to true.
    // The element completing the first true prefix belongs to the right side.
    template <class Pred>
    std::pair<Sequence, Sequence> split_prefix(Sequence&& seq, Pred&& pred) {
        check_owner(seq);
        if (seq.empty() || !pred(product(seq.root_)))
            return {std::move(seq), Sequence{}};
        S prefix = e();
        auto goes_left = [&](Index left) {
            S candidate = op(prefix, product(left));
            if (pred(static_cast<const S&>(candidate))) return true;
            prefix = std::move(candidate);
            return false;
        };
        return take_split(seq, split_before_true(seq.root_, goes_left));
    }

private:
    template <class T>
    class Pool {
        static constexpr unsigned block_shift = 10;
        static constexpr Index block_size = Index{1} << block_shift;
        union Slot {
            T value;
            Index next;
            Slot() noexcept {}
            ~Slot() {}
        };

        std::vector<std::unique_ptr<Slot[]>> blocks_;
        Index next_ = 1;
        Index free_ = 0;
#ifndef NDEBUG
        Index live_ = 0;
#endif

        Slot& slot(Index i) const noexcept {
            --i;
            return blocks_[i >> block_shift][i & (block_size - 1)];
        }

        void add_block() { blocks_.push_back(std::make_unique<Slot[]>(block_size)); }

    public:
        ~Pool() {
            assert(live_ == 0 && "WBLT must outlive its sequences");
        }

        void reserve(size_type count) {
            const size_type blocks = (count + block_size - 1) >> block_shift;
            blocks_.reserve(blocks);
            while (blocks_.size() < blocks) add_block();
        }

        template <class... Args>
        Index make(Args&&... args) {
            const Index i = free_ ? free_ : next_;
            if (i > max_slots) throw std::length_error("WBLT pool exhausted");
            if (((i - 1) >> block_shift) == blocks_.size()) add_block();
            Slot& s = slot(i);
            const Index next_free = free_ ? s.next : 0;
            try {
                ::new (static_cast<void*>(std::addressof(s.value)))
                    T(std::forward<Args>(args)...);
            } catch (...) {
                if (free_) s.next = next_free;
                throw;
            }
            if (free_) free_ = next_free;
            else ++next_;
#ifndef NDEBUG
            ++live_;
#endif
            return i;
        }

        void release(Index i) noexcept {
            Slot& s = slot(i);
            s.value.~T();
            s.next = free_;
            free_ = i;
#ifndef NDEBUG
            --live_;
#endif
        }

        T& operator[](Index i) noexcept { return slot(i).value; }
        const T& operator[](Index i) const noexcept { return slot(i).value; }
    };

    struct Branch {
        Index left, right, weight;
        S prod, last;
        std::optional<F> tag;

        Branch(Index l, Index r, Index w, S&& p, const S& tail)
            : left(l), right(r), weight(w), prod(std::move(p)), last(tail) {}
    };

    Pool<S> leaves_;
    Pool<Branch> branches_;

    template <class Value>
    Index make_leaf(Value&& value) {
        return leaf_bit | leaves_.make(std::forward<Value>(value));
    }

    static bool is_leaf(Index t) noexcept { return (t & leaf_bit) != 0; }

    Index weight(Index t) const noexcept {
        return is_leaf(t) ? 1 : branches_[t].weight;
    }

    const S& product(Index t) const noexcept {
        return is_leaf(t) ? leaves_[t ^ leaf_bit] : branches_[t].prod;
    }

    const S& last_element(Index t) const noexcept {
        return is_leaf(t) ? leaves_[t ^ leaf_bit] : branches_[t].last;
    }

    void append_elements(Index t, const F* pending, std::vector<S>& out) const {
        if (is_leaf(t)) {
            const S& value = leaves_[t ^ leaf_bit];
            if (pending) out.push_back(mapping(*pending, value));
            else out.push_back(value);
            return;
        }
        const Branch& b = branches_[t];
        if (b.tag) {
            if (pending) {
                // An unpushed ancestor action is newer than the local tag.
                const F combined = composition(*pending, *b.tag);
                append_elements(b.left, std::addressof(combined), out);
                append_elements(b.right, std::addressof(combined), out);
                return;
            }
            pending = std::addressof(*b.tag);
        }
        append_elements(b.left, pending, out);
        append_elements(b.right, pending, out);
    }

    void apply_to(Index t, const F& f) {
        if (is_leaf(t)) {
            S& value = leaves_[t ^ leaf_bit];
            value = mapping(f, static_cast<const S&>(value));
        } else {
            Branch& b = branches_[t];
            b.prod = mapping(f, static_cast<const S&>(b.prod));
            b.last = mapping(f, static_cast<const S&>(b.last));
            if (b.tag) *b.tag = composition(f, static_cast<const F&>(*b.tag));
            else b.tag.emplace(f);
        }
    }

    void push(Branch& b) {
        if (b.tag) {
            apply_to(b.left, *b.tag);
            apply_to(b.right, *b.tag);
            b.tag.reset();
        }
    }

    static bool heavier(Index a, Index b) noexcept { return a > 3ULL * b; }

    Index make_branch(Index l, Index r, Index lw, Index rw) {
        assert(!heavier(lw, rw) && !heavier(rw, lw));
        return branches_.make(l, r, lw + rw, op(product(l), product(r)), last_element(r));
    }

    void pull(Index t, Index l, Index r, Index lw, Index rw) {
        Branch& b = branches_[t];
        assert(!b.tag && !heavier(lw, rw) && !heavier(rw, lw));
        b.left = l;
        b.right = r;
        b.weight = lw + rw;
        b.prod = op(product(l), product(r));
        b.last = last_element(r);
    }

    // BB[1/4] join: descend on the heavy side and rotate on the way back.
    // This is weight-balanced BST join with weight measured in leaves and
    // with the separator stored as a branch instead of a sequence element.
    Index join(Index l, Index r, Index spare = 0) {
        const Index lw = weight(l), rw = weight(r);
        if (heavier(lw, rw)) {
            Branch& b = branches_[l];
            push(b);
            const Index outer = b.left;
            const Index inner = join(b.right, r, spare);
            return rebalance<true>(l, outer, inner);
        }
        if (heavier(rw, lw)) {
            Branch& b = branches_[r];
            push(b);
            const Index outer = b.right;
            const Index inner = join(l, b.left, spare);
            return rebalance<false>(r, outer, inner);
        }
        if (!spare) return make_branch(l, r, lw, rw);
        pull(spare, l, r, lw, rw);
        return spare;
    }

    template <bool Left>
    void pull_ordered(Index t, Index a, Index b, Index aw, Index bw) {
        if constexpr (Left) pull(t, a, b, aw, bw);
        else pull(t, b, a, bw, aw);
    }

    // 'inner' is the child that grew in join. Mirror the rotation for a join
    // that descended into the right input instead of the left input.
    template <bool Left>
    Index rebalance(Index t, Index outer, Index inner) {
        const Index ow = weight(outer), iw = weight(inner);
        if (!heavier(iw, ow)) {
            pull_ordered<Left>(t, outer, inner, ow, iw);
            return t;
        }
        Branch& b = branches_[inner];
        push(b);
        const Index near = Left ? b.left : b.right;
        const Index far = Left ? b.right : b.left;
        const Index nw = weight(near), fw = weight(far);
        if (!heavier(ow, nw) && !heavier(nw, ow) &&
            !heavier(ow + nw, fw) && !heavier(fw, ow + nw)) {
            pull_ordered<Left>(t, outer, near, ow, nw);
            pull_ordered<Left>(inner, t, far, ow + nw, fw);
            return inner;
        }
        Branch& mid = branches_[near];
        push(mid);
        const Index a = Left ? mid.left : mid.right;
        const Index c = Left ? mid.right : mid.left;
        const Index aw = weight(a), cw = weight(c);
        pull_ordered<Left>(t, outer, a, ow, aw);
        pull_ordered<Left>(inner, c, far, cw, fw);
        pull_ordered<Left>(near, t, inner, ow + aw, cw + fw);
        return near;
    }

    using Roots = std::pair<Index, Index>;

    // Called only for an interior cut. Reuse one branch per join, and
    // release exactly the branch at the cut.
    Roots split_at(Index t, Index k) {
        Branch& b = branches_[t];
        push(b);
        const Index l = b.left, r = b.right, lw = weight(l);
        if (k < lw) {
            auto [a, c] = split_at(l, k);
            return {a, join(c, r, t)};
        }
        if (k > lw) {
            auto [a, c] = split_at(r, k - lw);
            return {join(l, a, t), c};
        }
        branches_.release(t);
        return {l, r};
    }

    // The last element (or its inclusive prefix) is already known to be
    // true. Descend and split on the same pass, with no second rank search.
    template <class GoLeft>
    Roots split_before_true(Index t, GoLeft& goes_left) {
        if (is_leaf(t)) return {0, t};
        Branch& b = branches_[t];
        push(b);
        const Index l = b.left, r = b.right;
        if (goes_left(l)) {
            auto [a, c] = split_before_true(l, goes_left);
            if (!a) return {0, t};
            return {a, join(c, r, t)};
        }
        auto [a, c] = split_before_true(r, goes_left);
        if (!a) {
            branches_.release(t);
            return {l, c};
        }
        return {join(l, a, t), c};
    }

    template <class RandomIt>
    Index build_nodes(RandomIt first, Index n) {
        if (n == 1) return make_leaf(*first);
        const Index k = n / 2;
        const Index l = build_nodes(first, k);
        Index r = 0;
        try {
            r = build_nodes(first + k, n - k);
            return make_branch(l, r, k, n - k);
        } catch (...) {
            destroy(l);
            if (r) destroy(r);
            throw;
        }
    }

    void destroy(Index t) noexcept {
        if (is_leaf(t)) {
            leaves_.release(t ^ leaf_bit);
        } else {
            const Branch& b = branches_[t];
            destroy(b.left);
            destroy(b.right);
            branches_.release(t);
        }
    }

    void check_owner(const Sequence& seq) const {
        if (!seq.empty() && seq.pool_ != this)
            throw std::invalid_argument("WBLT sequence belongs to another pool");
    }

    std::pair<Sequence, Sequence> take_split(Sequence& seq, Roots roots) noexcept {
        seq.root_ = 0;
        return {Sequence(this, roots.first), Sequence(this, roots.second)};
    }
};

#endif  // WBLT_HPP
