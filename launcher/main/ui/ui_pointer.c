/*=============================================================================
 * ui_pointer - see ui_pointer.h.
 *===========================================================================*/
#include "ui/ui_pointer.h"

static ui_pointer_event_t make(ui_pointer_kind_t kind, int x, int y)
{
    return (ui_pointer_event_t){ .kind = kind, .x = x, .y = y };
}

/* No bounds check: ui_pointer_step()'s max < UI_POINTER_MAX_EVENTS guard
 * already rejected any buffer too small for the most this can ever write. */
static int emit(ui_pointer_event_t *out, int n, ui_pointer_kind_t kind, int x, int y)
{
    out[n] = make(kind, x, y);
    return n + 1;
}

int ui_pointer_step(ui_pointer_t *p, const input_t *input, ui_pointer_event_t *out, int max)
{
    if (max < UI_POINTER_MAX_EVENTS) {
        return 0;
    }

    int n = 0;

    if (input->pressed) {
        /* Frame 1 of a tap: position only, so hover resolves before a
         * control can be pressed - see ui.h's touch-to-mouse comment. */
        p->press_pending = true;
        p->press_x = input->x;
        p->press_y = input->y;
        n = emit(out, n, UI_POINTER_MOVE, p->press_x, p->press_y);

        if (input->released) {
            /* Resolved inside one frame - too fast for the hover frame to
             * ever get its own DOWN. Still a real tap, so still needs one. */
            n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
            p->press_pending = false;
            p->down = false;
        }
        return n;
    }

    if (p->press_pending) {
        /* Frame 2: the press lands on whatever the hover frame resolved. */
        n = emit(out, n, UI_POINTER_MOVE, p->press_x, p->press_y);
        n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
        p->press_pending = false;
        p->down = true;

        if (input->released) {
            /* The finger lifted before this DOWN frame arrived on its own -
             * same "still needs a real down/up pair" case as above, just
             * spread across two frames instead of one. */
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
            p->down = false;
        }
        return n;
    }

    if (input->released) {
        n = emit(out, n, UI_POINTER_MOVE, input->x, input->y);
        if (p->down) {
            /* Only a DOWN we actually emitted needs a matching UP - a
             * finger already on the glass when the UI opened never got
             * one (see input->down below), so its lift must stay silent. */
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
        }
        p->down = false;
        return n;
    }

    if (input->down) {
        /* A drag reads its position here every frame in between; also
         * covers a finger already down when the UI opened - no `pressed`
         * edge was ever seen for it, so no DOWN is synthesized for it
         * either, only this move. */
        n = emit(out, n, UI_POINTER_MOVE, input->x, input->y);
        return n;
    }

    /* Nothing down, nothing pending: park the pointer off-screen so no
     * control sits hovered. */
    n = emit(out, n, UI_POINTER_MOVE, -1, -1);
    return n;
}
