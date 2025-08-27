#include <iostream>

class Base {
public:
    virtual void Fo() = 0;
    ~Base() {}  // Невиртуальный
};

class Derived : public Base {
public:
    Derived() { data = new int[1000]; }  // Выделение памяти
    void Fo() final {}
    ~Derived() { delete[] data; }  // Освобождение в деструкторе наследника
private:
    int* data;
};

int main() {
    Base* obj = new Derived();
    delete obj;  // Утечка, т.к. вызывается деструктор Base, а не Derived
    return 0;
}