#include <stack>

enum class ValueType {
    String, Float,
    Int, Struct
};

struct Value {
    ValueType type;
    void* content;

    ~Value() {
        switch (type) {
            case ValueType::String:
                
        }
    }
};

struct Stack {
    
};