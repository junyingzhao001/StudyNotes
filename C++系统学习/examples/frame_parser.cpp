#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

class FrameParser {
public:
    void push(std::string bytes) {
        buffer_ += bytes;
    }

    std::vector<std::string> takeFrames() {
        std::vector<std::string> frames;
        while (buffer_.size() >= 4) {
            const std::uint32_t size = readSize();
            if (size > 1024) throw std::runtime_error{"frame too large"};
            if (buffer_.size() < 4 + size) break;
            frames.push_back(buffer_.substr(4, size));
            buffer_.erase(0, 4 + size);
        }
        return frames;
    }

private:
    std::uint32_t readSize() const {
        std::uint32_t value{};
        for (int index = 0; index < 4; ++index) {
            value = (value << 8) |
                static_cast<unsigned char>(buffer_[index]);
        }
        return value;
    }

    std::string buffer_;
};

int main() {
    FrameParser parser;
    parser.push(std::string{"\0\0", 2});
    parser.push(std::string{"\0\5hello\0\0\0\3bye", 14});
    for (const auto& frame : parser.takeFrames()) {
        std::cout << frame << '\n';
    }
}
