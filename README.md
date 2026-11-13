# WBLT

`wblt.hpp` 是一个 C++17 单头文件，实现了支持延迟作用的叶权平衡树。树叶按序保存元素；每个分支缓存子树大小、按原顺序计算的积和末元素。积不要求可交换。

```cpp
#include "wblt.hpp"

struct S { long long sum, len; };
struct F { long long a, b; };

S op(S x, S y) { return {x.sum + y.sum, x.len + y.len}; }
S e() { return {0, 0}; }
S mapping(F f, S x) { return {f.a * x.sum + f.b * x.len, x.len}; }
F composition(F f, F g) { return {f.a * g.a, f.a * g.b + f.b}; }
F id() { return {1, 0}; }

int main() {
    WBLT<S, op, e, F, mapping, composition, id> tree(4);
    auto a = tree.singleton({1, 1});
    auto b = tree.build({{2, 1}, {3, 1}, {4, 1}});
    auto seq = tree.merge(std::move(a), std::move(b));
    seq.apply({2, 1});  // [3, 5, 7, 9]
    auto values = seq.to_vector();
    assert(values.size() == 4 && values[0].sum == 3 && values[3].sum == 9);

    auto [l, r] = tree.split(std::move(seq), 2);  // [3, 5] | [7, 9]
    seq = tree.merge(std::move(l), std::move(r));
    auto [low, high] = tree.split_by(std::move(seq), [](S x) {
        return x.sum >= 7;
    });  // [3, 5] | [7, 9]
    seq = tree.merge(std::move(low), std::move(high));
    auto [before, after] = tree.split_prefix(std::move(seq), [](S prefix) {
        return prefix.sum >= 8;
    });  // [3] | [5, 7, 9]

    assert(before.prod().sum == 3 && after.prod().sum == 21);
}
```

模板参数与 `atcoder::lazy_segtree` 同序，`composition(f, g)` 表示先作用 `g` 再作用 `f`。要求 `op` 有结合律和幺元 `e()`，并且对每个作用 `f`，`mapping(f, op(x, y)) == op(mapping(f, x), mapping(f, y))`。延迟标记用“无标记”表示没有待下推的作用，因此末尾的 `id` 可以省略，`F` 不需要默认构造或比较相等。原地操作中的代数运算和 `S`、`F` 的相关拷贝、移动及赋值应不抛异常。

| 操作 | 语义 | 时间 |
| --- | --- | --- |
| `tree.singleton(value)` | 从一个 `S` 直接创建长度为 1 的序列 | 摊还 O(1)；容量已预留时 O(1) |
| `tree.build(v)` 或 `tree.build(first, last)` | 从 `std::vector<S>` 或随机访问迭代器区间建树 | O(n) |
| `tree.merge(std::move(l), std::move(r))` | 将 `r` 接到 `l` 后面 | O(1 + log(max(n,m) / min(n,m)))；空序列 O(1) |
| `tree.split(std::move(seq), k)` | 左侧保留前 `k` 个元素；允许 `k = 0` 或 `k = n` | O(log(n + 1)) |
| `tree.split_by(std::move(seq), pred)` | `pred(element)` 为假的元素留在左侧，从首个为真的元素开始是右侧 | O(log(n + 1)) |
| `tree.split_prefix(std::move(seq), pred)` | 对每个非空前缀的积判定；使判定首次为真的那个元素属于右侧 | O(log(n + 1)) |
| `seq.apply(f)` | 对整个序列施加 `f`；空序列保持为空 | O(1) |
| `seq.prod()` | 返回整个序列的积；空序列返回 `e()` | O(1) |
| `seq.to_vector()` | 按顺序返回施加了所有延迟作用的元素；不修改原序列，空序列返回空向量 | O(n) |
| `seq.size()`、`seq.empty()` | 查询长度或是否为空 | O(1) |

两种谓词分裂都要求判定沿序列从假到真单调变化，也允许全假或全真。复杂度假定用户定义的运算、判定和类型拷贝等是 O(1)。每个分支的左右叶数之比至多为 3；拼接沿较重一侧下降并旋转恢复平衡，分裂沿一条路径下降并复用原有分支。

每个 `WBLT` 实例管理自己的节点池，必须比从它建出的非空 `Sequence` 活得更久。拼接和分裂成功后会清空传入的句柄；两棵非空树只有来自同一个节点池才可以拼接。析构一个仍持有 `n` 个元素的句柄需要 O(n)，释放的槽位会被复用。池按块增长并保留已分配的块，直到池析构。已知同时存活元素数量上限时，可以用构造函数或 `reserve(n)` 提前分配，从而在该上限内避免池扩容；未预留时，建树和拼接的分配成本按摊还计算。单个池最多同时存放 2³¹ − 1 个元素。

参考：[AtCoder Library: Lazy Segtree](https://atcoder.github.io/ac-library/production/document_en/lazysegtree.html)；[Joinable Parallel Balanced Binary Trees](https://www.cs.cmu.edu/~blelloch/papers/3512769.pdf) §4.3。

运行测试：

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic tests/wblt_test.cpp -o /tmp/wblt-test
/tmp/wblt-test
```
