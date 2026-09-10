#include <iostream>
#include <string>

class Session {
public:
    explicit Session(const std::string& name) : name_(name) {
        std::cout << "open " << name_ << '\n';
    }
    ~Session() { std::cout << "close " << name_ << '\n'; }
    void greet() const { std::cout << "hello " << name_ << '\n'; }
private:
    std::string name_;
};

int main() {
    std::cout << "before scope" << '\n';
    {
        const Session session{"C++"};
        session.greet();
    }
    std::cout << "after scope" << '\n';
}
