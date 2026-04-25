#include <core/SkPath.h>

int main()
{
    SkPath path;
    path.moveTo(50, 50);
    path.lineTo(150, 50);
    path.lineTo(100, 150);
    path.close();
}
