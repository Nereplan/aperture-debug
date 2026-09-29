#include "TextInputModel.h"

#include <algorithm>
#include <utility>

namespace Aperture {

namespace {

char16_t LowerAscii(char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + (u'a' - u'A')) : c; }
} // namespace

TextInputModel::TextInputModel(Text initial, std::size_t maxLength) : maxLength_(maxLength) {
    text_ = TruncateToLength(NormalizeSingleLine(initial), maxLength_);
    anchor_ = caret_ = text_.size();
}

void TextInputModel::SetText(Text text, bool resetHistory) {
    CancelComposition();
    Text normalized = TruncateToLength(NormalizeSingleLine(text), maxLength_);
    if (resetHistory) { text_ = std::move(normalized); anchor_ = caret_ = text_.size(); ClearHistory(); return; }
    Snapshot before{ text_, anchor_, caret_ };
    if (text_ == normalized) { anchor_ = caret_ = text_.size(); return; }
    text_ = std::move(normalized);
    anchor_ = caret_ = text_.size();
    PushUndo(before);
    redo_.clear();
}

void TextInputModel::SetMaxLength(std::size_t maxLength) {
    CancelComposition();
    maxLength_ = maxLength;
    Text clipped = TruncateToLength(text_, maxLength_);
    if (clipped == text_) { NormalizeState(); return; }
    Snapshot before{ text_, anchor_, caret_ };
    text_ = std::move(clipped);
    NormalizeState();
    PushUndo(before);
    redo_.clear();
}

TextInputModel::Range TextInputModel::GetSelection() const { return SelectionForText(); }

TextInputModel::Text TextInputModel::GetSelectedText() const {
    const Range range = SelectionForText();
    return text_.substr(range.start, range.Length());
}

void TextInputModel::SetCaret(std::size_t utf16Index, bool extendSelection) {
    CancelComposition();
    const std::size_t boundary = NormalizeBoundary(text_, utf16Index);
    if (!extendSelection) anchor_ = boundary;
    caret_ = boundary;
}

void TextInputModel::SetSelection(std::size_t start, std::size_t end) {
    CancelComposition();
    anchor_ = NormalizeBoundary(text_, start);
    caret_ = NormalizeBoundary(text_, end);
}

void TextInputModel::SelectAll() { CancelComposition(); anchor_ = 0; caret_ = text_.size(); }

bool TextInputModel::MoveLeft(bool extendSelection, bool byWord) {
    CancelComposition();
    const Range range = SelectionForText();
    if (!extendSelection && !range.Empty()) { anchor_ = caret_ = range.start; return true; }
    const std::size_t destination = byWord ? MoveWordLeftFrom(caret_) : PreviousCodePointBoundary(text_, caret_);
    const bool changed = destination != caret_ || (!extendSelection && anchor_ != destination);
    if (extendSelection) caret_ = destination;
    else anchor_ = caret_ = destination;
    return changed;
}

bool TextInputModel::MoveRight(bool extendSelection, bool byWord) {
    CancelComposition();
    const Range range = SelectionForText();
    if (!extendSelection && !range.Empty()) { anchor_ = caret_ = range.end; return true; }
    const std::size_t destination = byWord ? MoveWordRightFrom(caret_) : NextCodePointBoundary(text_, caret_);
    const bool changed = destination != caret_ || (!extendSelection && anchor_ != destination);
    if (extendSelection) caret_ = destination;
    else anchor_ = caret_ = destination;
    return changed;
}

bool TextInputModel::MoveHome(bool extendSelection) {
    CancelComposition();
    const bool changed = caret_ != 0 || (!extendSelection && anchor_ != 0);
    if (extendSelection) caret_ = 0;
    else anchor_ = caret_ = 0;
    return changed;
}

bool TextInputModel::MoveEnd(bool extendSelection) {
    CancelComposition();
    const std::size_t destination = text_.size();
    const bool changed = caret_ != destination || (!extendSelection && anchor_ != destination);
    if (extendSelection) caret_ = destination;
    else anchor_ = caret_ = destination;
    return changed;
}

bool TextInputModel::InsertText(const Text& value) { return ReplaceSelection(value); }

bool TextInputModel::ReplaceSelection(const Text& value) {
    CancelComposition();
    const Range range = SelectionForText();
    return ApplyReplacement(range.start, range.end, value);
}

bool TextInputModel::DeleteBackward(bool byWord) {
    CancelComposition();
    const Range range = SelectionForText();
    if (!range.Empty()) return ApplyReplacement(range.start, range.end, {});
    const std::size_t start = byWord ? MoveWordLeftFrom(caret_) : PreviousCodePointBoundary(text_, caret_);
    if (start == caret_) return false;
    return ApplyReplacement(start, caret_, {});
}

bool TextInputModel::DeleteForward(bool byWord) {
    CancelComposition();
    const Range range = SelectionForText();
    if (!range.Empty()) return ApplyReplacement(range.start, range.end, {});
    const std::size_t end = byWord ? MoveWordRightFrom(caret_) : NextCodePointBoundary(text_, caret_);
    if (end == caret_) return false;
    return ApplyReplacement(caret_, end, {});
}

bool TextInputModel::CopySelection(const ClipboardCallbacks& clipboard) const {
    if (!HasSelection()) return false;
    if (clipboard.writeText) clipboard.writeText(GetSelectedText());
    return true;
}

bool TextInputModel::CutSelection(const ClipboardCallbacks& clipboard) {
    if (!HasSelection()) return false;
    if (clipboard.writeText) clipboard.writeText(GetSelectedText());
    return ApplyReplacement(SelectionForText().start, SelectionForText().end, {});
}

bool TextInputModel::HandleCtrlShortcut(char16_t key, const ClipboardCallbacks& clipboard) {
    switch (LowerAscii(key)) {
    case u'a': SelectAll(); return true;
    case u'c': CopySelection(clipboard); return true;
    case u'x': CutSelection(clipboard); return true;
    case u'v':
        if (clipboard.readText) InsertText(clipboard.readText());
        return true;
    case u'z': Undo(); return true;
    case u'y': Redo(); return true;
    default: return false;
    }
}

bool TextInputModel::Undo() {
    CancelComposition();
    if (undo_.empty()) return false;
    const Snapshot current{ text_, anchor_, caret_ };
    if (undoLimit_ != 0) { redo_.push_back(current); if (redo_.size() > undoLimit_) redo_.erase(redo_.begin()); }
    const Snapshot target = undo_.back();
    undo_.pop_back();
    Restore(target);
    return true;
}

bool TextInputModel::Redo() {
    CancelComposition();
    if (redo_.empty()) return false;
    const Snapshot current{ text_, anchor_, caret_ };
    if (undoLimit_ != 0) { undo_.push_back(current); if (undo_.size() > undoLimit_) undo_.erase(undo_.begin()); }
    const Snapshot target = redo_.back();
    redo_.pop_back();
    Restore(target);
    return true;
}

void TextInputModel::ClearHistory() { undo_.clear(); redo_.clear(); }

void TextInputModel::StartComposition() {
    if (compositionActive_) return;
    compositionActive_ = true;
    const Range range = SelectionForText();
    compositionStart_ = range.start;
    compositionEnd_ = range.end;
    compositionOriginalAnchor_ = anchor_;
    compositionOriginalCaret_ = caret_;
    compositionText_.clear();
    compositionCaret_ = 0;
}

void TextInputModel::UpdateComposition(const Text& composition, std::size_t compositionCaret) {
    if (!compositionActive_) StartComposition();
    compositionText_ = NormalizeAndFitReplacement(composition, compositionStart_, compositionEnd_);
    if (compositionCaret == kNoIndex) compositionCaret = compositionText_.size();
    compositionCaret_ = NormalizeBoundary(compositionText_, std::min(compositionCaret, compositionText_.size()));
}

bool TextInputModel::CommitComposition(const Text& committedText) {
    if (!compositionActive_) return ReplaceSelection(committedText);
    const bool changed = ApplyReplacement(compositionStart_, compositionEnd_, committedText);
    compositionActive_ = false;
    compositionText_.clear();
    compositionStart_ = compositionEnd_ = compositionCaret_ = 0;
    return changed;
}

bool TextInputModel::CommitComposition() { return CommitComposition(compositionText_); }

void TextInputModel::CancelComposition() {
    if (!compositionActive_) return;
    compositionActive_ = false;
    compositionText_.clear();
    compositionStart_ = compositionEnd_ = compositionCaret_ = 0;
    anchor_ = NormalizeBoundary(text_, compositionOriginalAnchor_);
    caret_ = NormalizeBoundary(text_, compositionOriginalCaret_);
}

void TextInputModel::EndComposition() { CancelComposition(); }

TextInputModel::Range TextInputModel::GetCompositionRange() const {
    if (!compositionActive_) return {};
    return { compositionStart_, compositionStart_ + compositionText_.size() };
}

std::size_t TextInputModel::GetDisplayCaret() const { return compositionActive_ ? compositionStart_ + compositionCaret_ : caret_; }

TextInputModel::Text TextInputModel::GetDisplayText() const {
    if (!compositionActive_) return text_;
    Text display;
    display.reserve(text_.size() - (compositionEnd_ - compositionStart_) + compositionText_.size());
    display.append(text_, 0, compositionStart_);
    display.append(compositionText_);
    display.append(text_, compositionEnd_, std::u16string::npos);
    return display;
}

bool TextInputModel::IsHighSurrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }

bool TextInputModel::IsLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

std::size_t TextInputModel::NormalizeBoundary(const Text& text, std::size_t index) {
    index = std::min(index, text.size());
    if (index > 0 && index < text.size() && IsHighSurrogate(text[index - 1]) && IsLowSurrogate(text[index])) { return index - 1; }
    return index;
}

std::size_t TextInputModel::NextCodePointBoundary(const Text& text, std::size_t index) {
    index = NormalizeBoundary(text, index);
    if (index >= text.size()) return text.size();
    if (IsHighSurrogate(text[index]) && index + 1 < text.size() && IsLowSurrogate(text[index + 1])) return index + 2;
    return index + 1;
}

std::size_t TextInputModel::PreviousCodePointBoundary(const Text& text, std::size_t index) {
    index = NormalizeBoundary(text, index);
    if (index == 0) return 0;
    --index;
    if (index > 0 && IsLowSurrogate(text[index]) && IsHighSurrogate(text[index - 1])) return index - 1;
    return index;
}

TextInputModel::Text TextInputModel::NormalizeSingleLine(const Text& value) {
    Text normalized;
    normalized.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char16_t c = value[i];
        if (c == u'\r') {
            if (i + 1 < value.size() && value[i + 1] == u'\n') ++i;
            normalized.push_back(u' ');
        } else if (c == u'\n' || c == u'\v' || c == u'\f' || c == 0x2028 || c == 0x2029) {
            normalized.push_back(u' ');
        } else { normalized.push_back(c); }
    }
    return normalized;
}

bool TextInputModel::IsWhitespace(char32_t codePoint) {
    return codePoint == U' ' || codePoint == U'\t' || codePoint == U'\n' || codePoint == U'\r' ||
        codePoint == U'\v' || codePoint == U'\f' || codePoint == 0x00A0 || codePoint == 0x3000;
}

bool TextInputModel::DecodePrevious(const Text& text, std::size_t boundary, std::size_t& start, char32_t& codePoint) {
    boundary = NormalizeBoundary(text, boundary);
    if (boundary == 0) return false;
    std::size_t index = boundary - 1;
    if (IsLowSurrogate(text[index]) && index > 0 && IsHighSurrogate(text[index - 1])) {
        const char32_t high = text[index - 1];
        const char32_t low = text[index];
        codePoint = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
        start = index - 1;
    } else { codePoint = text[index]; start = index; }
    return true;
}

bool TextInputModel::DecodeNext(const Text& text, std::size_t boundary, std::size_t& end, char32_t& codePoint) {
    boundary = NormalizeBoundary(text, boundary);
    if (boundary >= text.size()) return false;
    const char32_t first = text[boundary];
    if (IsHighSurrogate(static_cast<char16_t>(first)) && boundary + 1 < text.size() && IsLowSurrogate(text[boundary + 1])) {
        codePoint = 0x10000 + ((first - 0xD800) << 10) + (text[boundary + 1] - 0xDC00);
        end = boundary + 2;
    } else { codePoint = first; end = boundary + 1; }
    return true;
}

TextInputModel::Text TextInputModel::TruncateToLength(const Text& value, std::size_t maxLength) {
    if (maxLength == 0 || value.size() <= maxLength) return value;
    Text clipped = value.substr(0, maxLength);
    // A code-unit limit can end immediately after a high surrogate. Drop the incomplete lead unit rather than exposing malformed UTF-16 to
    // the renderer or host search field.
    if (!clipped.empty() && IsHighSurrogate(clipped.back())) clipped.pop_back();
    clipped.resize(NormalizeBoundary(clipped, clipped.size()));
    return clipped;
}

TextInputModel::Range TextInputModel::SelectionForText() const {
    const std::size_t a = NormalizeBoundary(text_, anchor_);
    const std::size_t c = NormalizeBoundary(text_, caret_);
    return a <= c ? Range{ a, c } : Range{ c, a };
}

std::size_t TextInputModel::MoveWordLeftFrom(std::size_t index) const {
    std::size_t position = NormalizeBoundary(text_, index);
    std::size_t start = 0;
    char32_t codePoint = 0;
    while (DecodePrevious(text_, position, start, codePoint) && IsWhitespace(codePoint)) position = start;
    while (DecodePrevious(text_, position, start, codePoint) && !IsWhitespace(codePoint)) position = start;
    return position;
}

std::size_t TextInputModel::MoveWordRightFrom(std::size_t index) const {
    std::size_t position = NormalizeBoundary(text_, index);
    std::size_t end = 0;
    char32_t codePoint = 0;
    while (DecodeNext(text_, position, end, codePoint) && IsWhitespace(codePoint)) position = end;
    while (DecodeNext(text_, position, end, codePoint) && !IsWhitespace(codePoint)) position = end;
    return position;
}

void TextInputModel::NormalizeState() { anchor_ = NormalizeBoundary(text_, anchor_); caret_ = NormalizeBoundary(text_, caret_); }

void TextInputModel::PushUndo(const Snapshot& snapshot) {
    if (undoLimit_ == 0) return;
    undo_.push_back(snapshot);
    if (undo_.size() > undoLimit_) undo_.erase(undo_.begin());
}

void TextInputModel::Restore(const Snapshot& snapshot) {
    text_ = snapshot.text;
    anchor_ = snapshot.anchor;
    caret_ = snapshot.caret;
    NormalizeState();
}

TextInputModel::Text TextInputModel::NormalizeAndFitReplacement(const Text& value, std::size_t start, std::size_t end) const {
    Text normalized = NormalizeSingleLine(value);
    if (maxLength_ == 0) return normalized;
    start = NormalizeBoundary(text_, start);
    end = NormalizeBoundary(text_, end);
    if (end < start) std::swap(start, end);
    const std::size_t remaining = text_.size() - (end - start);
    const std::size_t available = remaining >= maxLength_ ? 0 : maxLength_ - remaining;
    return TruncateToLength(normalized, available);
}

bool TextInputModel::ApplyReplacement(std::size_t start, std::size_t end, const Text& value) {
    start = NormalizeBoundary(text_, start);
    end = NormalizeBoundary(text_, end);
    if (end < start) std::swap(start, end);
    const Text replacement = NormalizeAndFitReplacement(value, start, end);
    Text candidate;
    candidate.reserve(text_.size() - (end - start) + replacement.size());
    candidate.append(text_, 0, start);
    candidate.append(replacement);
    candidate.append(text_, end, std::u16string::npos);
    const bool changed = candidate != text_;
    if (changed) {
        PushUndo({ text_, anchor_, caret_ });
        text_ = std::move(candidate);
        redo_.clear();
    }
    anchor_ = caret_ = start + replacement.size();
    NormalizeState();
    return changed;
}

} // namespace Aperture
