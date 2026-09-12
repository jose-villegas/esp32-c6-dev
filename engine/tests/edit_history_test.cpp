#include "core/edit_history.h"

#include <gtest/gtest.h>

namespace {

struct FakeDocument {
    int value = 0;
    bool dirty = false;

    void
    set_dirty(bool next_dirty) {
        dirty = next_dirty;
    }
};

struct SameValue {
    bool
    operator()(const FakeDocument& first, const FakeDocument& second) const {
        return first.value == second.value;
    }
};

using History = EditHistory<FakeDocument, SameValue>;

TEST(EditHistory, StartsCleanWithoutUndoOrRedo) {
    FakeDocument document;
    History history(document);

    EXPECT_FALSE(history.can_undo());
    EXPECT_FALSE(history.can_redo());
    EXPECT_FALSE(history.undo(document));
    EXPECT_FALSE(history.redo(document));
    EXPECT_FALSE(document.dirty);
}

TEST(EditHistory, CommitsUndoAndRedo) {
    FakeDocument document;
    History history(document);
    document.value = 7;
    history.commit(document);

    EXPECT_TRUE(history.can_undo());
    EXPECT_TRUE(document.dirty);
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.value, 0);
    EXPECT_FALSE(document.dirty);
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(document.value, 7);
    EXPECT_TRUE(document.dirty);
}

TEST(EditHistory, IgnoresEquivalentCommit) {
    FakeDocument document;
    History history(document);

    document.dirty = true;
    history.commit(document);

    EXPECT_FALSE(history.can_undo());
    EXPECT_FALSE(document.dirty);
}

TEST(EditHistory, DropsRedoBranchAfterNewCommit) {
    FakeDocument document;
    History history(document);
    document.value = 1;
    history.commit(document);
    document.value = 2;
    history.commit(document);
    ASSERT_TRUE(history.undo(document));

    document.value = 3;
    history.commit(document);

    EXPECT_FALSE(history.can_redo());
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.value, 1);
}

TEST(EditHistory, DirtyStateTracksSavedRevision) {
    FakeDocument document;
    History history(document);
    document.value = 1;
    history.mark_saved(document);

    EXPECT_FALSE(document.dirty);
    ASSERT_TRUE(history.undo(document));
    EXPECT_TRUE(document.dirty);
    ASSERT_TRUE(history.redo(document));
    EXPECT_FALSE(document.dirty);
}

TEST(EditHistory, DiscardedSavedBranchStaysDirty) {
    FakeDocument document;
    History history(document);
    document.value = 1;
    history.mark_saved(document);
    ASSERT_TRUE(history.undo(document));
    document.value = 2;
    history.commit(document);

    EXPECT_TRUE(document.dirty);
    ASSERT_TRUE(history.undo(document));
    EXPECT_TRUE(document.dirty);
}

} // namespace
