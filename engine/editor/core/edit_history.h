#pragma once

#include <cstddef>
#include <utility>
#include <vector>

template <typename Document, typename Equivalent>
class EditHistory {
  public:
    explicit EditHistory(const Document& document, Equivalent equivalent = {})
        : equivalent_(std::move(equivalent)), states_{document} {}

    void
    commit(Document& document) {
        if (equivalent_(states_[cursor_], document)) {
            sync_dirty(document);
            return;
        }
        if (cursor_ + 1 < states_.size()) {
            states_.erase(states_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), states_.end());
            if (saved_cursor_ > cursor_) {
                saved_cursor_ = no_saved_cursor;
            }
        }
        states_.push_back(document);
        cursor_++;
        sync_dirty(document);
    }

    bool
    undo(Document& document) {
        commit(document);
        if (cursor_ == 0) {
            return false;
        }
        document = states_[--cursor_];
        sync_dirty(document);
        return true;
    }

    bool
    redo(Document& document) {
        if (cursor_ + 1 >= states_.size()) {
            return false;
        }
        document = states_[++cursor_];
        sync_dirty(document);
        return true;
    }

    void
    mark_saved(Document& document) {
        commit(document);
        saved_cursor_ = cursor_;
        sync_dirty(document);
    }

    bool
    can_undo() const {
        return cursor_ > 0;
    }

    bool
    can_redo() const {
        return cursor_ + 1 < states_.size();
    }

  private:
    void
    sync_dirty(Document& document) const {
        document.set_dirty(cursor_ != saved_cursor_);
    }

    static constexpr std::size_t no_saved_cursor = static_cast<std::size_t>(-1);
    Equivalent equivalent_;
    std::vector<Document> states_;
    std::size_t cursor_ = 0;
    std::size_t saved_cursor_ = 0;
};
