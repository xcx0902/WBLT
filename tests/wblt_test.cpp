#include "../wblt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

namespace {

using U = std::uint64_t;

// A sum and an order-sensitive rolling hash of the same sequence.
struct Value {
    U sum, hash, power, ones, count;
};

struct F {
    U a, b;
    F(U a_, U b_) : a(a_), b(b_) {}
};

Value op(const Value& a, const Value& b) {
    return {a.sum + b.sum, a.hash * b.power + b.hash,
            a.power * b.power, a.ones * b.power + b.ones, a.count + b.count};
}
Value e() { return {0, 0, 1, 0, 0}; }
constexpr U base = 1099511628211ULL;
Value element(U x) { return {x, x, base, 1, 1}; }

Value mapping(const F& f, const Value& x) {
    return {f.a * x.sum + f.b * x.count, f.a * x.hash + f.b * x.ones,
            x.power, x.ones, x.count};
}
F composition(const F& f, const F& g) { return {f.a * g.a, f.a * g.b + f.b}; }
F id() { return {1, 0}; }

using Tree = WBLT<Value, op, e, F, mapping, composition, id>;
using Sequence = Tree::Sequence;

void check(bool ok, int line) {
    if (!ok) {
        std::cerr << "WBLT check failed at line " << line << '\n';
        std::abort();
    }
}
#define CHECK(...) check((__VA_ARGS__), __LINE__)

Sequence build(Tree& tree, const std::vector<U>& plain) {
    std::vector<Value> values;
    values.reserve(plain.size());
    for (U x : plain) values.push_back(element(x));
    return tree.build(std::move(values));
}

void verify(const Sequence& seq, const std::vector<U>& plain) {
    const Value got = seq.prod();
    U sum = 0, hash = 0, power = 1, ones = 0;
    for (U x : plain) {
        sum += x;
        hash = hash * base + x;
        power *= base;
        ones = ones * base + 1;
    }
    CHECK(seq.size() == plain.size());
    CHECK(seq.empty() == plain.empty());
    CHECK(got.count == plain.size());
    CHECK(got.sum == sum && got.hash == hash);
    CHECK(got.power == power && got.ones == ones);
}

void apply(Sequence& seq, std::vector<U>& plain, const F& f) {
    seq.apply(f);
    for (auto& x : plain) x = f.a * x + f.b;
}

U multiply(U a, U b) { return a * b; }
U one() { return 1; }
U zero_if(bool f, U x) { return f ? 0 : x; }
bool combine_actions(bool f, bool g) { return f || g; }

void empty_identity() {
    WBLT<U, multiply, one, bool, zero_if, combine_actions> tree;
    auto empty = tree.build({});
    // A distributive action is allowed to change e() on an actual element.
    empty.apply(true);
    CHECK(empty.prod() == 1);
    auto seq = tree.build({1, 2, 3});
    seq.apply(true);
    auto [l, r] = tree.split(std::move(seq), 1);
    CHECK(l.prod() == 0 && r.prod() == 0);
}

void boundaries() {
    Tree tree(4096);
    auto empty = tree.build(std::vector<Value>{});
    empty.apply({0, 123});
    verify(empty, {});
    const auto never_called = [](const Value&) -> bool { std::abort(); };
    auto [empty_l, empty_r] = tree.split_by(std::move(empty), never_called);
    auto [empty_pl, empty_pr] = tree.split_prefix(std::move(empty_l), never_called);
    verify(empty_pl, {});
    verify(empty_pr, {});

    std::vector<U> plain(2049);
    std::iota(plain.begin(), plain.end(), U{1});
    auto seq = build(tree, plain);
    seq = tree.merge(std::move(empty_r), std::move(seq));
    // The order of these first two pending actions is observable.
    apply(seq, plain, {1, 3});
    apply(seq, plain, {2, 1});
    for (std::size_t k = 0; k <= plain.size(); ++k) {
        apply(seq, plain, {1, 1});
        const U threshold = k < plain.size() ? plain[k] : plain.back() + 1;
        auto [l, r] = tree.split_by(std::move(seq), [&](const Value& x) {
            CHECK(x.count == 1);
            return x.sum >= threshold;
        });
        CHECK(seq.empty());
        const auto cut = plain.begin() + static_cast<std::ptrdiff_t>(k);
        verify(l, {plain.begin(), cut});
        verify(r, {cut, plain.end()});
        seq = tree.merge(std::move(l), std::move(r));
        CHECK(l.empty() && r.empty());
    }

    auto singleton = tree.build(std::vector<Value>{element(8)});
    auto [single_l, single_r] =
        tree.split_prefix(std::move(singleton), [](const Value& x) { return x.sum >= 8; });
    verify(single_l, {});
    verify(single_r, {8});
    auto [single, tail] = tree.split(std::move(single_r), 1);
    verify(single, {8});
    verify(tail, {});
}

void randomized() {
    Tree tree;
    std::array<Sequence, 24> seq;
    std::array<std::vector<U>, 24> plain;
    for (U seed : std::array<U, 4>{3, 87, 721, 10201}) {
        std::mt19937_64 rng(seed);
        for (int step = 0; step < 30000; ++step) {
            const auto a = rng() % seq.size(), b = rng() % seq.size();
            if (a == b) continue;
            const auto n = plain[a].size();
            switch (rng() % 6) {
            case 0:
                if (n > 20) break;
                plain[a].resize(rng() % 120);
                for (auto& x : plain[a]) x = rng() % 100;
                seq[a] = build(tree, plain[a]);
                break;
            case 1:
            case 2:
                apply(seq[a], plain[a], {rng() % 3 != 0, rng() % 30});
                break;
            case 3:
                seq[a] = tree.merge(std::move(seq[a]), std::move(seq[b]));
                plain[a].insert(plain[a].end(), plain[b].begin(), plain[b].end());
                plain[b].clear();
                break;
            case 4:
            case 5: {
                auto k = rng() % (n + 1);
                if (step % 2 == 0) {
                    auto [l, r] = tree.split(std::move(seq[a]), k);
                    seq[a] = std::move(l);
                    seq[b] = std::move(r);
                } else {
                    U target = 0;
                    for (std::size_t i = 0; i <= k && i < n; ++i)
                        target += plain[a][i];
                    if (k == n) ++target;
                    k = 0;
                    U sum = 0;
                    while (k < n && (sum += plain[a][k]) < target) ++k;
                    auto [l, r] = tree.split_prefix(std::move(seq[a]), [&](const Value& x) {
                        return x.sum >= target;
                    });
                    seq[a] = std::move(l);
                    seq[b] = std::move(r);
                }
                plain[b].assign(plain[a].begin() + static_cast<std::ptrdiff_t>(k),
                                plain[a].end());
                plain[a].resize(k);
                break;
            }
            }
            verify(seq[a], plain[a]);
            verify(seq[b], plain[b]);
        }
    }
}

void skewed_merges() {
    constexpr std::size_t n = 65536;
    Tree tree(n);
    Sequence seq;
    std::vector<U> plain(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto value = element(i);
        auto one = tree.build(&value, &value + 1);
        if (i % 2) {
            seq = tree.merge(std::move(seq), std::move(one));
            plain[n / 2 + i / 2] = i;
        } else {
            seq = tree.merge(std::move(one), std::move(seq));
            plain[n / 2 - 1 - i / 2] = i;
        }
    }
    verify(seq, plain);
    // Keep splitting and rejoining distant weights; cut all the way to leaves.
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = i % 2 ? n - 1 : 1;
        auto [l, r] = tree.split(std::move(seq), k);
        CHECK(l.size() == k && r.size() == n - k);
        CHECK((k == 1 ? l : r).prod().sum == (k == 1 ? plain.front() : plain.back()));
        seq = tree.merge(std::move(l), std::move(r));
    }
    verify(seq, plain);
}

}  // namespace

int main() {
    empty_identity();
    boundaries();
    randomized();
    skewed_merges();
    std::cout << "WBLT tests passed\n";
}
