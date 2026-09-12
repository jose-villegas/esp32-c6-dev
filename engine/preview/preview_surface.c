#include "engine/preview_surface.h"

#include <stddef.h>

static uint16_t
rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static void
fill(engine_preview_surface_t* surface, int x, int y, int w, int h, uint16_t color) {
    if (!surface || !surface->pixels || w <= 0 || h <= 0) {
        return;
    }

    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > surface->width ? surface->width : x + w;
    const int y1 = y + h > surface->height ? surface->height : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            surface->pixels[(size_t)py * (size_t)surface->width + (size_t)px] = color;
        }
    }
}

void
engine_preview_render_transport_test(engine_preview_surface_t* surface) {
    if (!surface || !surface->pixels || surface->width <= 0 || surface->height <= 0) {
        return;
    }

    const uint16_t background = rgb565(5, 24, 38);
    const uint16_t panel = rgb565(13, 47, 65);
    const uint16_t cyan = rgb565(91, 235, 225);
    const uint16_t violet = rgb565(163, 124, 255);
    const uint16_t amber = rgb565(255, 184, 77);
    const int margin = 16;
    const int gap = 12;

    fill(surface, 0, 0, surface->width, surface->height, background);
    fill(surface, margin, margin, surface->width - margin * 2, 32, panel);
    fill(surface, margin, margin, 72, 2, cyan);

    const int content_y = margin + 32 + gap;
    const int content_h = surface->height - content_y - margin;
    if (surface->width > surface->height) {
        const int card_w = (surface->width - margin * 2 - gap * 2) / 3;
        fill(surface, margin, content_y, card_w, content_h, panel);
        fill(surface, margin + card_w + gap, content_y, card_w, content_h, panel);
        fill(surface, margin + (card_w + gap) * 2, content_y, card_w, content_h, panel);
        fill(surface, margin, content_y, card_w, 3, cyan);
        fill(surface, margin + card_w + gap, content_y, card_w, 3, violet);
        fill(surface, margin + (card_w + gap) * 2, content_y, card_w, 3, amber);
    } else {
        const int card_h = (content_h - gap * 2) / 3;
        fill(surface, margin, content_y, surface->width - margin * 2, card_h, panel);
        fill(surface, margin, content_y + card_h + gap, surface->width - margin * 2, card_h, panel);
        fill(surface, margin, content_y + (card_h + gap) * 2, surface->width - margin * 2, card_h, panel);
        fill(surface, margin, content_y, 3, card_h, cyan);
        fill(surface, margin, content_y + card_h + gap, 3, card_h, violet);
        fill(surface, margin, content_y + (card_h + gap) * 2, 3, card_h, amber);
    }
}
