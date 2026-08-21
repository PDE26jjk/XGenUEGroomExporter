#ifndef XGCORE_XGEN_MEMORY_INTERNAL_H
#define XGCORE_XGEN_MEMORY_INTERNAL_H

#include <cstddef>
#include <cstdint>

// In-order iteration over an MSVC std::_Tree (std::map / std::multimap) using
// the node layout verified on Maya 2019/2025 (see the IDA report):
//   node:  left @ +0x00, parent @ +0x08, right @ +0x10, color @ +0x18,
//          key   @ +0x20, value @ +0x40
//   tree:  _Myhead @ +0x00, _Mysize @ +0x08
// The sentinel ("head") node participates in the tree links but holds no
// value; iteration starts at head->left and ends when tree_next returns head.
// Header-only so the unit tests can exercise the same code paths.
namespace xgcore {
namespace mem {

// The sentinel ("head") node has _Isnil set at +0x19; real nodes do not.
inline bool node_is_sentinel(const uint8_t* node) {
  return node != nullptr && node[0x19] != 0;
}

inline const uint8_t* tree_leftmost(const uint8_t* head) {
  return *reinterpret_cast<const uint8_t* const*>(head + 0x00);
}

inline const uint8_t* tree_next(const uint8_t* node, const uint8_t* head) {
  // Mirrors MSVC _Tree_unchecked_const_iterator::operator++: a right child
  // that is the sentinel means "no right subtree" (missing children of leaf
  // nodes point back to the sentinel), so climb; otherwise descend to the
  // smallest node of the right subtree.
  const uint8_t* right =
      *reinterpret_cast<const uint8_t* const*>(node + 0x10);
  if (right != nullptr && node_is_sentinel(right)) {
    const uint8_t* parent =
        *reinterpret_cast<const uint8_t* const*>(node + 0x08);
    while (parent != nullptr && !node_is_sentinel(parent) &&
           node ==
               *reinterpret_cast<const uint8_t* const*>(parent + 0x10)) {
      node = parent;
      parent = *reinterpret_cast<const uint8_t* const*>(node + 0x08);
    }
    return parent != nullptr ? parent : head;
  }
  if (right == nullptr) return head;  // defensive
  node = right;
  const uint8_t* left =
      *reinterpret_cast<const uint8_t* const*>(node + 0x00);
  while (left != nullptr && !node_is_sentinel(left)) {
    node = left;
    left = *reinterpret_cast<const uint8_t* const*>(node + 0x00);
  }
  return node;
}

// Value pointer inside a multimap node whose value is a std::shared_ptr<T>:
// the object pointer is the first member of the shared_ptr at +0x40.
template <typename T>
inline T* node_shared_value(const uint8_t* node) {
  return *reinterpret_cast<T* const*>(node + 0x40);
}

}  // namespace mem
}  // namespace xgcore

#endif  // XGCORE_XGEN_MEMORY_INTERNAL_H
