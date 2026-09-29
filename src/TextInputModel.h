#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Aperture {

// A small, renderer-independent single-line editor. Positions are UTF-16 code-unit offsets, but every movement/deletion operation
// normalizes them to Unicode code-point boundaries so a valid surrogate pair is never split. Combining marks remain individual code points
// (grapheme clustering belongs to the caller/rendering layer).
class TextInputModel {
public:
    using Text = std::u16string;
    static constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();
    static constexpr std::size_t kDefaultMaxLength = 4096;
    struct Range {
        std::size_t start = 0;
        std::size_t end = 0;
        bool Empty() const { return start == end; }
        std::size_t Length() const { return end >= start ? end - start : 0; }
    };
    // Clipboard access is deliberately supplied by the host. This keeps the model testable and allows App to use either Win32 clipboard
    // APIs or a platform abstraction without coupling this class to a window handle.
    struct ClipboardCallbacks { std::function<void(const Text&)> writeText; std::function<Text()> readText; };
    explicit TextInputModel(Text initial = {}, std::size_t maxLength = kDefaultMaxLength);
    const Text& GetText() const { return text_; }
    void SetText(Text text, bool resetHistory = true);
    // Length is measured in UTF-16 code units. Zero means unlimited; any
    // nonzero limit is enforced without retaining a dangling surrogate.
    void SetMaxLength(std::size_t maxLength);
    std::size_t GetCaret() const { return caret_; }
    std::size_t GetAnchor() const { return anchor_; }
    Range GetSelection() const;
    bool HasSelection() const { return caret_ != anchor_; }
    Text GetSelectedText() const;
    // SetCaret and SetSelection accept UTF-16 offsets from mouse/edit-control hit testing and snap offsets that fall inside a surrogate
    // pair outward to a safe code-point boundary.
    void SetCaret(std::size_t utf16Index, bool extendSelection = false);
    void SetSelection(std::size_t start, std::size_t end);
    void SetCaretFromMouseIndex(std::size_t utf16Index, bool extendSelection = false) { SetCaret(utf16Index, extendSelection); }
    void SelectAll();
    bool MoveLeft(bool extendSelection = false, bool byWord = false);
    bool MoveRight(bool extendSelection = false, bool byWord = false);
    bool MoveHome(bool extendSelection = false);
    bool MoveEnd(bool extendSelection = false);
    bool InsertText(const Text& value);
    bool ReplaceSelection(const Text& value);
    bool DeleteBackward(bool byWord = false);
    bool DeleteForward(bool byWord = false);
    bool CopySelection(const ClipboardCallbacks& clipboard) const;
    bool CutSelection(const ClipboardCallbacks& clipboard);
    // key is the unmodified ASCII shortcut character (a/c/v/x/y/z). The
    // caller decides whether Ctrl is down and calls this only for Ctrl+key.
    bool HandleCtrlShortcut(char16_t key, const ClipboardCallbacks& clipboard = {});
    bool Undo();
    bool Redo();
    void ClearHistory();
    // IME composition is temporary display state. GetText(), selection, and
    // undo history contain only committed text until CommitComposition runs.
    void StartComposition();
    void UpdateComposition(const Text& composition, std::size_t compositionCaret = kNoIndex);
    bool CommitComposition(const Text& committedText);
    bool CommitComposition();
    void CancelComposition();
    void EndComposition();
    bool IsComposing() const { return compositionActive_; }
    // Range is measured in GetDisplayText() coordinates (the captured selection is replaced by the temporary composition for display).
    Range GetCompositionRange() const;
    std::size_t GetDisplayCaret() const;
    Text GetDisplayText() const;
    // Helpers are public for host hit testing and focused unit tests.
    static bool IsHighSurrogate(char16_t c);
    static bool IsLowSurrogate(char16_t c);
    static std::size_t NextCodePointBoundary(const Text& text, std::size_t index);
    static std::size_t PreviousCodePointBoundary(const Text& text, std::size_t index);
    static std::size_t NormalizeBoundary(const Text& text, std::size_t index);
    static Text NormalizeSingleLine(const Text& value);

private:
    struct Snapshot { Text text; std::size_t anchor = 0; std::size_t caret = 0; };
    Text text_;
    std::size_t maxLength_ = kDefaultMaxLength;
    std::size_t undoLimit_ = 128;
    std::size_t anchor_ = 0;
    std::size_t caret_ = 0;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool compositionActive_ = false;
    Text compositionText_;
    std::size_t compositionStart_ = 0;
    std::size_t compositionEnd_ = 0;
    std::size_t compositionCaret_ = 0;
    std::size_t compositionOriginalAnchor_ = 0;
    std::size_t compositionOriginalCaret_ = 0;
    static bool IsWhitespace(char32_t codePoint);
    static bool DecodePrevious(const Text& text, std::size_t boundary, std::size_t& start, char32_t& codePoint);
    static bool DecodeNext(const Text& text, std::size_t boundary, std::size_t& end, char32_t& codePoint);
    static Text TruncateToLength(const Text& value, std::size_t maxLength);
    Range SelectionForText() const;
    std::size_t MoveWordLeftFrom(std::size_t index) const;
    std::size_t MoveWordRightFrom(std::size_t index) const;
    void NormalizeState();
    void PushUndo(const Snapshot& snapshot);
    void Restore(const Snapshot& snapshot);
    bool ApplyReplacement(std::size_t start, std::size_t end, const Text& value);
    Text NormalizeAndFitReplacement(const Text& value, std::size_t start, std::size_t end) const;
};

} // namespace Aperture
