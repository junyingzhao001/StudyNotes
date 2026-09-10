#include <iostream>
#include <memory>

template<typename T>
T larger(T left, T right) { return left > right ? left : right; }

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

class Rectangle : public Shape {
public:
    Rectangle(double width, double height) : width_(width), height_(height) {}
    double area() const override { return width_ * height_; }
private:
    double width_;
    double height_;
};

int main() {
    std::cout << "larger: " << larger(3, 7) << '\n';
    std::unique_ptr<Shape> shape = std::make_unique<Rectangle>(3.0, 4.0);
    std::cout << "area: " << shape->area() << '\n';
}
