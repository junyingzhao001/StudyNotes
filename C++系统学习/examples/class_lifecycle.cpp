#include <iostream>
#include <string>

class Session {
public:
    explicit Session(std::string name) : name_(std::move(name)) {
        std::cout << "open " << name_ << '\n';
    }

    ~Session() {
        std::cout << "close " << name_ << '\n';
    }

    void use() const {
        std::cout << "use " << name_ << '\n';
    }

private:
    std::string name_;
};

int main() {
    std::cout << "before\n";
    {
        Session session{"database"};
        session.use();
    }
    std::cout << "after\n";
}
