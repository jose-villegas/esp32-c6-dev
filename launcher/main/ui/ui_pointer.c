/* ui_pointer - see ui_pointer.h. */
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
        /* First of the hover frames: position only. See
         * UI_POINTER_HOVER_FRAMES for why a press cannot simply be fed
         * here, and why one such frame is not enough. */
        p->press_stage = 1;
        p->press_x = input->x;
        p->press_y = input->y;
        n = emit(out, n, UI_POINTER_MOVE, p->press_x, p->press_y);

        if (input->released) {
            /* Resolved inside one frame - too fast for the hover frames to
             * play out. Still a real tap, so it still owes a down/up pair,
             * and feeding both here leaves microui the same
             * mouse_down-already-clear frame it resolved taps on before
             * the pointer ever learned to hold. */
            n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
            p->press_stage = 0;
            p->down = false;
        }
        return n;
    }

    if (p->press_stage > 0) {
        n = emit(out, n, UI_POINTER_MOVE, p->press_x, p->press_y);

        const bool last_hover_frame = (p->press_stage >= UI_POINTER_HOVER_FRAMES);
        if (last_hover_frame) {
            n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
            p->press_stage = 0;
            p->down = true;
        } else {
            p->press_stage++;
        }

        if (input->released) {
            /* Lifted mid-sequence. A press that never got its DOWN still
             * owes one, or the tap vanishes entirely. */
            if (!p->down) {
                n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
            }
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
            p->press_stage = 0;
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
