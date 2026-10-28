#include <core/SkPath.h>
#include <core/SkPathBuilder.h>

int main() {
    SkPathBuilder builder;
    builder.moveTo(50, 50);
    builder.lineTo(150, 50);
    builder.lineTo(100, 150);
    builder.close();
    SkPath path = builder.detach();
}
