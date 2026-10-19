#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace Aperture::Reorder {
// Returns the final slot of an unaffected item after a single-item move.
inline int Slot(int index, int from, int to) {
    if (index == from) return to;
    if (from < to && index > from && index <= to) return index - 1;
    if (from > to && index >= to && index < from) return index + 1;
    return index;
}
// Returns the logical slot whose center is nearest to the dragged item's center. Using centers makes the destination change only after the
// item crosses the midpoint between neighboring slots, independent of where the user grabbed it.
inline int LinearTargetFromCenter(float center, float firstCenter, float pitch, int count) {
    if (count <= 0 || pitch <= 0.0f) return -1;
    const int slot = static_cast<int>(std::floor(((center - firstCenter) / pitch) + 0.5f));
    return std::clamp(slot, 0, count - 1);
}
inline int GridTargetFromCenter(float centerX, float centerY, float firstCenterX, float firstCenterY, float pitchX, float pitchY,
                                int columns, int count) {
    if (count <= 0 || columns <= 0 || pitchX <= 0.0f || pitchY <= 0.0f) return -1;
    const int column = std::clamp( static_cast<int>(std::floor(((centerX - firstCenterX) / pitchX) + 0.5f)), 0, columns - 1);
    const int row = std::max(0, static_cast<int>(std::floor(((centerY - firstCenterY) / pitchY) + 0.5f)));
    return std::min(count - 1, row * columns + column);
}
inline float EdgeSpeed(float pointer, float top, float bottom, float scale) {
    float band = std::min(48.0f * scale, (bottom - top) * 0.25f);
    if (band <= 0 || pointer < top - band || pointer > bottom + band) return 0;
    float strength = pointer < top + band ? -(top + band - pointer) / band : pointer > bottom - band ? (pointer - bottom + band) / band : 0;
    strength = std::clamp(strength, -1.0f, 1.0f);
    return strength * std::abs(strength) * 620.0f * scale;
}
template<class T> bool Move(std::vector<T>& items, int from, int to) {
    if (from < 0 || to < 0 || from >= static_cast<int>(items.size()) || to >= static_cast<int>(items.size()) || from == to) return false;
    if (from < to) std::rotate(items.begin() + from, items.begin() + from + 1, items.begin() + to + 1);
    else std::rotate(items.begin() + to, items.begin() + from, items.begin() + from + 1);
    return true;
}
}
