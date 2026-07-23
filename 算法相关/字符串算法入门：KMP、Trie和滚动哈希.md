# 字符串算法入门：KMP、Trie和滚动哈希

字符串题常见难点是：

```text
匹配
前缀
重复
大量查询
```

最值得掌握的基础字符串算法有：

```text
KMP
Trie
滚动哈希
```

---

## 1. KMP 解决什么问题？

KMP 用来在文本串中查找模式串。

```text
text = "ababcabcacbab"
pattern = "abcac"
```

暴力匹配遇到失败时，会把模式串重新从头比较。

KMP 的优化是：

```text
失败时，不回退 text 指针，而是利用 pattern 自己的前后缀信息跳转。
```

---

## 2. next 数组是什么？

`next[i]` 表示：

```text
pattern[0..i] 这个前缀里，最长相等真前后缀的长度
```

例如：

```text
pattern = "abab"
```

`"abab"` 的最长相等真前后缀是：

```text
"ab"
```

长度是 2。

注意“真前后缀”不能等于字符串本身。以 `pattern = "ababaca"` 为例：

```text
字符:  a b a b a c a
下标:  0 1 2 3 4 5 6
next:  0 0 1 2 3 0 1
```

构建时的 `j` 既表示“当前已匹配前缀长度”，也表示下一次要比较的模式串下标。
失配后执行 `j = next[j - 1]`，相当于尝试更短的可复用前后缀；这个回退可能连续发生多次。

---

## 3. KMP 构建 next

```java
int[] buildNext(String p) {
    int n = p.length();
    int[] next = new int[n];

    int j = 0;
    for (int i = 1; i < n; i++) {
        while (j > 0 && p.charAt(i) != p.charAt(j)) {
            j = next[j - 1];
        }

        if (p.charAt(i) == p.charAt(j)) {
            j++;
        }

        next[i] = j;
    }

    return next;
}
```

匹配：

```java
int strStr(String text, String pattern) {
    if (pattern.length() == 0) return 0;

    int[] next = buildNext(pattern);
    int j = 0;

    for (int i = 0; i < text.length(); i++) {
        while (j > 0 && text.charAt(i) != pattern.charAt(j)) {
            j = next[j - 1];
        }

        if (text.charAt(i) == pattern.charAt(j)) {
            j++;
        }

        if (j == pattern.length()) {
            return i - pattern.length() + 1;
        }
    }

    return -1;
}
```

```mermaid
flowchart LR
  A["匹配失败"] --> B["不回退 text"]
  B --> C["根据 next 调整 pattern 指针"]
  C --> D["继续匹配"]
```

---

## 4. Trie 字典树

Trie 用来处理大量字符串的前缀查询。

适合：

```text
单词插入
单词查找
前缀查找
自动补全
敏感词匹配的基础结构
```

代码：

```java
class Trie {
    static class Node {
        Node[] children = new Node[26];
        boolean isWord;
    }

    private Node root = new Node();

    public void insert(String word) {
        Node cur = root;
        for (char c : word.toCharArray()) {
            int index = c - 'a';
            if (index < 0 || index >= 26) {
                throw new IllegalArgumentException("示例 Trie 只支持 a-z");
            }
            if (cur.children[index] == null) {
                cur.children[index] = new Node();
            }
            cur = cur.children[index];
        }
        cur.isWord = true;
    }

    public boolean search(String word) {
        Node node = find(word);
        return node != null && node.isWord;
    }

    public boolean startsWith(String prefix) {
        return find(prefix) != null;
    }

    private Node find(String s) {
        Node cur = root;
        for (char c : s.toCharArray()) {
            int index = c - 'a';
            if (index < 0 || index >= 26) {
                throw new IllegalArgumentException("示例 Trie 只支持 a-z");
            }
            if (cur.children[index] == null) {
                return null;
            }
            cur = cur.children[index];
        }
        return cur;
    }
}
```

---

## 5. 滚动哈希

滚动哈希用于快速比较子串。

核心思想：

```text
把字符串看成一个 base 进制数字
用前缀哈希快速算任意子串哈希
```

例如：

```text
hash("abc") = a * base^2 + b * base + c
```

定义：

```text
prefix[i + 1] = prefix[i] * base + value(s[i])
power[i + 1]  = power[i] * base
```

那么半开区间 `[left, right)` 的哈希为：

```text
hash(left, right) = prefix[right] - prefix[left] * power[right - left]
```

Java 示例（利用 `long` 的自然溢出作为模 `2^64` 运算）：

```java
class RollingHash {
    private static final long BASE = 911382323L;
    private final long[] prefix;
    private final long[] power;

    RollingHash(String s) {
        int n = s.length();
        prefix = new long[n + 1];
        power = new long[n + 1];
        power[0] = 1;

        for (int i = 0; i < n; i++) {
            prefix[i + 1] = prefix[i] * BASE + (s.charAt(i) + 1L);
            power[i + 1] = power[i] * BASE;
        }
    }

    // 返回 s[left, right) 的哈希
    long hash(int left, int right) {
        if (left < 0 || left > right || right >= prefix.length) {
            throw new IndexOutOfBoundsException();
        }
        return prefix[right] - prefix[left] * power[right - left];
    }
}
```

滚动哈希适合：

```text
重复子串
字符串匹配
快速比较两个子串是否相等
```

注意：哈希相等只表示“很可能相等”，并不是数学上的充分条件。需要绝对正确时，应在哈希命中后
再逐字符校验；双哈希只能进一步降低冲突概率，不能把概率降为零。

复杂度对比：

| 算法 | 预处理 | 单次查询/匹配 | 额外空间 |
|---|---:|---:|---:|
| KMP | `O(m)` | 文本匹配 `O(n)` | `O(m)` |
| Trie | 插入总字符数 `O(S)` | 查询 `O(L)` | `O(S × 字符集开销)` |
| 滚动哈希 | `O(n)` | 子串哈希 `O(1)` | `O(n)` |

---

## 6. 三者怎么选？

| 场景 | 算法 |
|---|---|
| 单模式串匹配 | KMP |
| 大量前缀查询 | Trie |
| 快速比较子串 | 滚动哈希 |
| 多模式串匹配 | Trie 的扩展 AC 自动机 |

---

## 7. 一句话总结

KMP：

```text
利用模式串自身的前后缀信息，避免重复匹配。
```

Trie：

```text
把公共前缀合并成树，适合前缀查询。
```

滚动哈希：

```text
把子串比较变成数字哈希比较。
```
