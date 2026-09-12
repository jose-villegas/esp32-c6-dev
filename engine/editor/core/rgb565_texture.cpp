#include "core/rgb565_texture.h"

#include <SDL.h>

#include <cstddef>

Rgb565Texture::Rgb565Texture(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {}

Rgb565Texture::~Rgb565Texture() { reset(); }

bool
Rgb565Texture::create(SDL_Renderer* renderer, std::string& error) {
    reset();
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width_, height_);
    if (!texture_ || SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest) != 0) {
        error = SDL_GetError();
        reset();
        return false;
    }
    error.clear();
    return true;
}

bool
Rgb565Texture::upload(std::string& error) {
    if (!texture_
        || SDL_UpdateTexture(texture_, nullptr, pixels_.data(), width_ * static_cast<int>(sizeof(uint16_t))) != 0) {
        error = SDL_GetError();
        return false;
    }
    error.clear();
    return true;
}

void
Rgb565Texture::reset() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
}

int
Rgb565Texture::width() const {
    return width_;
}

int
Rgb565Texture::height() const {
    return height_;
}

uint16_t*
Rgb565Texture::pixels() {
    return pixels_.data();
}

SDL_Texture*
Rgb565Texture::native_handle() const {
    return texture_;
}
