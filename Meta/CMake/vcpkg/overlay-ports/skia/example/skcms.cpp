#include <modules/skcms/skcms.h>

int main() {
    float src_pixels[4] = {0.8f, 0.3f, 0.2f, 1.0f};
    float dst_pixels[4] = {};
    
    skcms_Transform(
        src_pixels, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Unpremul, skcms_sRGB_profile(),
        dst_pixels, skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Unpremul, skcms_XYZD50_profile(),
        1
    );
}
