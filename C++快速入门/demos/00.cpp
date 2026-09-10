#include <iostream>
#include <string>

struct Note {
    std::string text;
};

void rename(Note& note) { note.text = "changed through reference"; }

int main() {
    Note original{"original"};
    Note copied = original;  // 复制出独立对象
    Note& alias = original;  // original 的别名
    Note* observer = &original; // 保存地址，只借用

    copied.text = "changed copy";
    rename(alias);

    std::cout << "original: " << observer->text << '\n';
    std::cout << "copy: " << copied.text << '\n';
}
