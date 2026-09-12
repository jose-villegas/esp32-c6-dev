#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

class Rgb565Texture {
  public:
    Rgb565Texture(int width, int height);
    ~Rgb565Texture();

    Rgb565Texture(const Rgb565Texture&) = delete;
    Rgb565Texture& operator=(const Rgb565Texture&) = delete;

    bool create(SDL_Renderer* renderer, std::string& error);
    bool upload(std::string& error);
    void reset();

    int width() const;
    int height() const;
    uint16_t* pixels();
    SDL_Texture* native_handle() const;

  private:
    int width_;
    int height_;
    std::vector<uint16_t> pixels_;
    SDL_Texture* texture_ = nullptr;
};
