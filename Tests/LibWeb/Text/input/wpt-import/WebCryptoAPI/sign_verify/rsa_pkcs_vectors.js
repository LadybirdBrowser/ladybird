
// rsa_pkcs_vectors.js

// Data for testing RSASSA-PKCS1-v1_5 with a 2048-bit modulus and 65537 public exponent.

// The following function returns an array of test vectors
// for the subtleCrypto encrypt method.
//
// Each test vector has the following fields:
//     name - a unique name for this vector
//     publicKeyBuffer - an arrayBuffer with the key data
//     publicKeyFormat - "spki" "jwk"
//     publicKey - a CryptoKey object for the keyBuffer. INITIALLY null! You must fill this in first to use it!
//     privateKeyBuffer - an arrayBuffer with the key data
//     privateKeyFormat - "pkcs8" or "jwk"
//     privateKey - a CryptoKey object for the keyBuffer. INITIALLY null! You must fill this in first to use it!
//     algorithm - the value of the AlgorithmIdentifier parameter to provide to encrypt
//     plaintext - the text to encrypt
//     signature - the expected signature
function getTestVectors() {
    var pkcs8 = new Uint8Array([48, 130, 4, 191, 2, 1, 0, 48, 13, 6, 9, 42, 134, 72, 134, 247, 13, 1, 1, 1, 5, 0, 4, 130, 4, 169, 48, 130, 4, 165, 2, 1, 0, 2, 130, 1, 1, 0, 211, 87, 96, 146, 230, 41, 87, 54, 69, 68, 231, 228, 35, 59, 123, 219, 41, 61, 178, 8, 81, 34, 196, 121, 50, 133, 70, 249, 240, 247, 18, 246, 87, 196, 177, 120, 104, 201, 48, 144, 140, 197, 148, 247, 237, 0, 192, 20, 66, 193, 175, 4, 194, 246, 120, 164, 139, 162, 200, 15, 209, 113, 62, 48, 181, 172, 80, 120, 122, 195, 81, 101, 137, 241, 113, 150, 127, 99, 134, 173, 163, 73, 0, 166, 187, 4, 238, 206, 164, 43, 240, 67, 206, 217, 160, 249, 77, 12, 192, 158, 145, 155, 157, 113, 102, 192, 138, 182, 206, 32, 70, 64, 174, 164, 196, 146, 13, 182, 216, 110, 185, 22, 208, 220, 192, 244, 52, 26, 16, 56, 4, 41, 231, 225, 3, 33, 68, 234, 148, 157, 232, 246, 192, 204, 191, 149, 250, 142, 146, 141, 112, 216, 163, 140, 225, 104, 219, 69, 246, 241, 52, 102, 61, 111, 101, 111, 92, 234, 188, 114, 93, 168, 192, 42, 171, 234, 170, 19, 172, 54, 167, 92, 192, 186, 225, 53, 223, 49, 20, 182, 101, 137, 199, 237, 60, 182, 21, 89, 174, 90, 56, 79, 22, 43, 250, 128, 219, 228, 97, 127, 134, 195, 241, 208, 16, 201, 79, 226, 201, 191, 1, 154, 110, 99, 179, 239, 192, 40, 212, 60, 238, 97, 28, 133, 236, 38, 60, 144, 108, 70, 55, 114, 198, 145, 27, 25, 238, 192, 150, 202, 118, 236, 94, 49, 225, 227, 2, 3, 1, 0, 1, 2, 130, 1, 1, 0, 139, 55, 92, 203, 135, 200, 37, 197, 255, 61, 83, 208, 9, 145, 110, 150, 65, 5, 126, 24, 82, 114, 39, 160, 122, 178, 38, 190, 16, 136, 129, 58, 59, 56, 187, 123, 72, 243, 119, 5, 81, 101, 250, 42, 147, 57, 210, 77, 198, 103, 213, 197, 186, 52, 39, 230, 164, 129, 23, 110, 172, 21, 255, 212, 144, 104, 49, 30, 28, 40, 59, 159, 58, 142, 12, 184, 9, 180, 99, 12, 80, 170, 143, 62, 69, 166, 11, 53, 158, 25, 191, 140, 187, 94, 202, 214, 78, 118, 31, 16, 149, 116, 63, 243, 106, 175, 92, 240, 236, 185, 127, 237, 173, 221, 166, 11, 91, 243, 93, 129, 26, 117, 184, 34, 35, 12, 250, 160, 25, 47, 173, 64, 84, 126, 39, 84, 72, 170, 51, 22, 191, 142, 43, 76, 224, 133, 79, 199, 112, 139, 83, 123, 162, 45, 19, 33, 11, 9, 174, 195, 122, 39, 89, 239, 192, 130, 161, 83, 27, 35, 169, 23, 48, 3, 125, 222, 78, 242, 107, 95, 150, 239, 220, 195, 159, 211, 76, 52, 90, 213, 28, 187, 228, 79, 229, 139, 138, 59, 78, 201, 151, 134, 108, 8, 109, 255, 27, 136, 49, 239, 10, 31, 234, 38, 60, 247, 218, 205, 3, 192, 76, 188, 194, 178, 121, 229, 127, 165, 185, 83, 153, 107, 251, 29, 214, 136, 23, 175, 127, 180, 44, 222, 247, 165, 41, 74, 87, 250, 194, 184, 173, 115, 159, 27, 2, 153, 2, 129, 129, 0, 251, 248, 51, 194, 198, 49, 201, 112, 36, 12, 142, 116, 133, 240, 106, 62, 162, 168, 72, 34, 81, 26, 134, 39, 221, 70, 78, 248, 175, 175, 113, 72, 209, 164, 37, 182, 184, 101, 125, 221, 82, 70, 131, 43, 142, 83, 48, 32, 197, 187, 181, 104, 133, 90, 106, 236, 62, 66, 33, 215, 147, 241, 220, 91, 47, 37, 132, 226, 65, 94, 72, 233, 162, 189, 41, 43, 19, 64, 49, 249, 156, 142, 180, 47, 192, 188, 208, 68, 155, 242, 44, 230, 222, 201, 112, 20, 239, 229, 172, 147, 235, 232, 53, 135, 118, 86, 37, 44, 187, 177, 108, 65, 91, 103, 177, 132, 210, 40, 69, 104, 162, 119, 213, 147, 53, 88, 92, 253, 2, 129, 129, 0, 214, 184, 206, 39, 199, 41, 93, 93, 22, 252, 53, 112, 237, 100, 200, 218, 147, 3, 250, 210, 148, 136, 193, 166, 94, 154, 215, 17, 249, 3, 112, 24, 125, 187, 253, 129, 49, 109, 105, 100, 139, 200, 140, 197, 200, 53, 81, 175, 255, 69, 222, 186, 207, 182, 17, 5, 247, 9, 228, 195, 8, 9, 185, 0, 49, 235, 214, 134, 36, 68, 150, 198, 246, 158, 105, 46, 189, 200, 20, 246, 66, 57, 244, 173, 21, 117, 110, 203, 120, 197, 165, 176, 153, 49, 219, 24, 48, 119, 197, 70, 163, 140, 76, 116, 56, 137, 173, 61, 62, 208, 121, 181, 98, 46, 208, 18, 15, 160, 225, 249, 59, 89, 61, 183, 216, 82, 224, 95, 2, 129, 128, 56, 135, 75, 157, 131, 247, 129, 120, 206, 45, 158, 252, 23, 92, 131, 137, 127, 214, 127, 48, 107, 191, 166, 159, 100, 238, 52, 35, 104, 206, 212, 124, 128, 195, 241, 206, 23, 122, 117, 141, 100, 186, 251, 12, 151, 134, 164, 66, 133, 250, 1, 205, 236, 53, 7, 205, 238, 125, 201, 183, 226, 178, 29, 60, 187, 204, 16, 14, 238, 153, 103, 132, 59, 5, 115, 41, 253, 204, 166, 41, 152, 237, 15, 17, 179, 140, 232, 176, 171, 199, 222, 57, 1, 124, 113, 207, 208, 174, 87, 84, 108, 85, 145, 68, 205, 208, 175, 208, 100, 95, 126, 168, 255, 7, 185, 116, 209, 237, 68, 253, 31, 142, 0, 245, 96, 191, 109, 69, 2, 129, 129, 0, 133, 41, 239, 144, 115, 207, 143, 123, 95, 249, 226, 26, 186, 223, 58, 65, 115, 211, 144, 6, 112, 223, 175, 89, 66, 106, 188, 223, 4, 147, 193, 61, 47, 29, 27, 70, 184, 36, 166, 172, 24, 148, 179, 217, 37, 37, 12, 24, 30, 52, 114, 193, 96, 120, 5, 110, 177, 154, 141, 40, 247, 31, 48, 128, 146, 117, 52, 129, 212, 148, 68, 253, 247, 140, 158, 166, 194, 68, 7, 220, 1, 142, 119, 211, 175, 239, 56, 91, 47, 247, 67, 158, 150, 35, 121, 65, 51, 45, 212, 70, 206, 190, 255, 219, 68, 4, 254, 79, 113, 89, 81, 97, 208, 22, 64, 44, 51, 77, 15, 87, 198, 26, 190, 79, 249, 244, 203, 249, 2, 129, 129, 0, 135, 216, 119, 8, 212, 103, 99, 228, 204, 190, 178, 209, 233, 113, 46, 91, 240, 33, 109, 112, 222, 148, 32, 165, 178, 6, 155, 116, 89, 185, 159, 93, 159, 127, 47, 173, 124, 215, 154, 174, 230, 122, 127, 154, 52, 67, 126, 60, 121, 168, 74, 240, 205, 141, 233, 223, 242, 104, 235, 12, 71, 147, 245, 1, 249, 136, 213, 64, 246, 211, 71, 92, 32, 121, 184, 34, 122, 35, 217, 104, 222, 196, 227, 198, 101, 3, 24, 113, 147, 69, 150, 48, 71, 43, 253, 182, 186, 29, 231, 134, 199, 151, 250, 111, 78, 166, 90, 42, 132, 25, 38, 47, 41, 103, 136, 86, 203, 115, 201, 189, 75, 200, 155, 94, 4, 27, 34, 119]);
    var spki = new Uint8Array([48, 130, 1, 34, 48, 13, 6, 9, 42, 134, 72, 134, 247, 13, 1, 1, 1, 5, 0, 3, 130, 1, 15, 0, 48, 130, 1, 10, 2, 130, 1, 1, 0, 211, 87, 96, 146, 230, 41, 87, 54, 69, 68, 231, 228, 35, 59, 123, 219, 41, 61, 178, 8, 81, 34, 196, 121, 50, 133, 70, 249, 240, 247, 18, 246, 87, 196, 177, 120, 104, 201, 48, 144, 140, 197, 148, 247, 237, 0, 192, 20, 66, 193, 175, 4, 194, 246, 120, 164, 139, 162, 200, 15, 209, 113, 62, 48, 181, 172, 80, 120, 122, 195, 81, 101, 137, 241, 113, 150, 127, 99, 134, 173, 163, 73, 0, 166, 187, 4, 238, 206, 164, 43, 240, 67, 206, 217, 160, 249, 77, 12, 192, 158, 145, 155, 157, 113, 102, 192, 138, 182, 206, 32, 70, 64, 174, 164, 196, 146, 13, 182, 216, 110, 185, 22, 208, 220, 192, 244, 52, 26, 16, 56, 4, 41, 231, 225, 3, 33, 68, 234, 148, 157, 232, 246, 192, 204, 191, 149, 250, 142, 146, 141, 112, 216, 163, 140, 225, 104, 219, 69, 246, 241, 52, 102, 61, 111, 101, 111, 92, 234, 188, 114, 93, 168, 192, 42, 171, 234, 170, 19, 172, 54, 167, 92, 192, 186, 225, 53, 223, 49, 20, 182, 101, 137, 199, 237, 60, 182, 21, 89, 174, 90, 56, 79, 22, 43, 250, 128, 219, 228, 97, 127, 134, 195, 241, 208, 16, 201, 79, 226, 201, 191, 1, 154, 110, 99, 179, 239, 192, 40, 212, 60, 238, 97, 28, 133, 236, 38, 60, 144, 108, 70, 55, 114, 198, 145, 27, 25, 238, 192, 150, 202, 118, 236, 94, 49, 225, 227, 2, 3, 1, 0, 1]);

    // plaintext
    var plaintext = new Uint8Array([95, 77, 186, 79, 50, 12, 12, 232, 118, 114, 90, 252, 229, 251, 210, 91, 248, 62, 90, 113, 37, 160, 140, 175, 231, 60, 62, 186, 196, 33, 119, 157, 249, 213, 93, 24, 12, 58, 233, 148, 38, 69, 225, 216, 47, 238, 140, 157, 41, 75, 60, 177, 160, 138, 153, 49, 32, 27, 60, 14, 129, 252, 71, 202, 207, 131, 21, 162, 175, 102, 50, 65, 19, 195, 182, 98, 48, 195, 70, 8, 196, 244, 89, 54, 52, 206, 2, 178, 103, 54, 34, 119, 240, 168, 64, 202, 116, 188, 61, 26, 98, 54, 149, 44, 94, 215, 170, 248, 168, 254, 203, 221, 250, 117, 132, 230, 151, 140, 234, 93, 42, 91, 159, 183, 241, 180, 140, 139, 11, 229, 138, 48, 82, 2, 117, 77, 131, 118, 16, 115, 116, 121, 60, 240, 38, 170, 238, 83, 0, 114, 125, 131, 108, 215, 30, 113, 179, 69, 221, 178, 228, 68, 70, 255, 197, 185, 1, 99, 84, 19, 137, 13, 145, 14, 163, 128, 152, 74, 144, 25, 16, 49, 50, 63, 22, 219, 204, 157, 107, 225, 104, 184, 72, 133, 56, 76, 160, 62, 18, 96, 10, 193, 194, 72, 2, 138, 243, 114, 108, 201, 52, 99, 136, 46, 168, 192, 42, 171]);

    // For verification tests.
    var signatures = {
        "sha-1": new Uint8Array([83, 46, 47, 27, 105, 204, 46, 232, 71, 46, 242, 143, 127, 54, 168, 26, 36, 205, 228, 238, 131, 133, 138, 125, 23, 5, 74, 195, 96, 44, 152, 221, 67, 46, 59, 54, 144, 68, 9, 53, 7, 43, 183, 192, 49, 230, 128, 112, 29, 25, 185, 124, 181, 81, 13, 134, 201, 190, 219, 231, 209, 192, 104, 57, 206, 237, 138, 59, 106, 201, 86, 65, 49, 196, 81, 43, 187, 171, 222, 35, 123, 77, 170, 41, 250, 13, 60, 151, 72, 123, 72, 168, 254, 233, 214, 59, 80, 86, 157, 198, 183, 209, 8, 80, 200, 50, 5, 89, 52, 59, 133, 55, 182, 18, 20, 167, 228, 84, 58, 113, 77, 101, 226, 28, 78, 71, 130, 148, 235, 66, 70, 206, 166, 104, 227, 81, 252, 224, 180, 225, 24, 199, 88, 190, 79, 220, 70, 199, 179, 34, 107, 191, 64, 181, 179, 149, 13, 98, 184, 189, 170, 79, 107, 183, 106, 48, 34, 43, 163, 39, 52, 237, 93, 244, 172, 141, 79, 255, 167, 85, 113, 5, 8, 122, 106, 207, 186, 91, 72, 81, 97, 99, 187, 145, 104, 100, 232, 44, 184, 97, 235, 145, 13, 207, 111, 26, 219, 173, 83, 153, 175, 212, 151, 251, 122, 251, 127, 117, 218, 131, 200, 5, 146, 234, 26, 222, 62, 56, 3, 180, 187, 104, 49, 185, 51, 41, 124, 15, 204, 195, 105, 55, 228, 96, 24, 121, 127, 202, 133, 148, 125, 41, 198, 162, 122, 129]),
        "sha-256": new Uint8Array([19, 48, 106, 186, 37, 29, 238, 82, 100, 89, 194, 131, 82, 164, 41, 205, 216, 85, 84, 199, 228, 166, 121, 10, 44, 110, 68, 180, 94, 187, 196, 160, 10, 10, 85, 36, 77, 98, 166, 13, 223, 14, 215, 212, 45, 119, 20, 241, 47, 120, 58, 198, 112, 157, 14, 39, 2, 219, 38, 146, 174, 170, 204, 90, 92, 219, 190, 193, 115, 25, 140, 170, 176, 209, 79, 232, 133, 208, 168, 218, 31, 22, 106, 150, 92, 158, 37, 212, 132, 112, 180, 136, 77, 92, 146, 164, 216, 68, 23, 68, 5, 86, 143, 74, 192, 52, 13, 246, 196, 16, 252, 68, 207, 126, 230, 213, 155, 166, 52, 249, 198, 36, 12, 150, 181, 154, 6, 252, 238, 255, 77, 210, 150, 34, 231, 249, 131, 174, 191, 0, 236, 242, 65, 241, 201, 18, 207, 213, 220, 110, 238, 185, 79, 157, 145, 97, 19, 232, 67, 169, 133, 85, 194, 66, 87, 248, 195, 237, 171, 31, 39, 131, 159, 140, 201, 169, 99, 232, 184, 84, 101, 165, 110, 193, 216, 118, 34, 90, 224, 1, 251, 212, 36, 71, 1, 226, 228, 125, 129, 181, 87, 132, 126, 56, 39, 227, 59, 54, 243, 245, 232, 254, 223, 164, 154, 13, 52, 208, 29, 189, 175, 132, 250, 92, 117, 47, 2, 2, 168, 202, 178, 196, 204, 44, 181, 7, 111, 101, 55, 217, 194, 45, 53, 55, 56, 233, 179, 156, 151, 2, 107, 5, 156, 233, 93, 137]),
        "sha-384": new Uint8Array([53, 79, 205, 28, 98, 226, 54, 45, 78, 139, 206, 223, 81, 80, 247, 178, 123, 236, 51, 171, 50, 162, 121, 117, 52, 90, 76, 140, 254, 178, 52, 102, 155, 196, 171, 175, 129, 231, 25, 221, 244, 193, 175, 174, 69, 67, 44, 183, 174, 185, 19, 60, 189, 135, 141, 231, 102, 232, 114, 98, 129, 120, 163, 58, 194, 10, 2, 138, 125, 140, 43, 100, 26, 92, 82, 59, 22, 187, 230, 94, 178, 11, 171, 51, 28, 152, 58, 150, 27, 174, 109, 230, 78, 107, 144, 119, 170, 137, 200, 70, 184, 214, 157, 207, 113, 1, 71, 141, 16, 117, 26, 59, 135, 178, 165, 222, 44, 166, 206, 113, 160, 191, 237, 127, 88, 122, 33, 110, 5, 58, 30, 83, 196, 162, 172, 233, 60, 38, 27, 68, 15, 236, 212, 51, 13, 215, 203, 215, 146, 184, 57, 133, 2, 178, 162, 8, 69, 164, 194, 145, 141, 135, 42, 172, 245, 11, 48, 39, 19, 87, 1, 154, 88, 174, 24, 129, 158, 117, 196, 142, 158, 248, 8, 16, 134, 15, 160, 73, 100, 119, 108, 160, 75, 32, 3, 41, 103, 77, 202, 83, 32, 180, 0, 245, 23, 134, 78, 113, 224, 135, 182, 139, 129, 223, 97, 62, 226, 75, 172, 52, 220, 242, 230, 69, 149, 225, 44, 121, 144, 112, 243, 247, 25, 217, 61, 120, 67, 182, 149, 146, 52, 108, 252, 32, 198, 187, 249, 49, 7, 242, 121, 214, 32, 126, 198, 87]),
        "sha-512": new Uint8Array([98, 41, 183, 8, 151, 248, 98, 11, 99, 84, 135, 205, 74, 169, 150, 38, 152, 49, 255, 41, 49, 210, 135, 20, 240, 30, 88, 177, 101, 241, 8, 44, 49, 152, 184, 244, 81, 120, 140, 253, 62, 213, 154, 120, 248, 52, 209, 28, 226, 133, 209, 5, 28, 66, 165, 206, 160, 34, 127, 222, 254, 41, 52, 68, 194, 81, 142, 190, 92, 176, 5, 91, 234, 75, 88, 6, 240, 235, 161, 182, 101, 2, 42, 99, 190, 68, 192, 136, 254, 154, 210, 99, 37, 215, 159, 124, 65, 237, 151, 249, 9, 205, 76, 162, 131, 40, 228, 196, 169, 222, 141, 166, 124, 53, 220, 28, 133, 183, 30, 214, 255, 170, 249, 157, 116, 178, 184, 142, 159, 95, 5, 167, 50, 246, 104, 140, 153, 59, 88, 160, 237, 53, 232, 240, 161, 6, 212, 232, 177, 179, 96, 227, 52, 65, 92, 116, 46, 148, 103, 88, 35, 219, 15, 210, 94, 34, 207, 247, 166, 51, 92, 112, 225, 147, 35, 93, 205, 164, 138, 221, 104, 88, 98, 107, 217, 99, 17, 230, 15, 126, 94, 164, 73, 27, 108, 30, 98, 72, 175, 225, 43, 187, 213, 79, 136, 105, 176, 67, 165, 176, 68, 69, 98, 129, 63, 10, 152, 179, 0, 53, 111, 48, 110, 107, 120, 58, 41, 243, 190, 201, 124, 164, 14, 162, 0, 98, 202, 184, 146, 110, 197, 217, 106, 163, 135, 204, 132, 130, 26, 109, 114, 184, 234, 18, 110, 125])
    };

    var vectors = [
        {
            name: "RSASSA-PKCS1-v1_5 with SHA-1",
            publicKeyBuffer: spki,
            publicKeyFormat: "spki",
            privateKey: null,
            privateKeyBuffer: pkcs8,
            privateKeyFormat: "pkcs8",
            publicKey: null,
            algorithm: {name: "RSASSA-PKCS1-v1_5"},
            hash: "SHA-1",
            plaintext: plaintext,
            signature: signatures["sha-1"]
        },
        {
            name: "RSASSA-PKCS1-v1_5 with SHA-256",
            publicKeyBuffer: spki,
            publicKeyFormat: "spki",
            privateKey: null,
            privateKeyBuffer: pkcs8,
            privateKeyFormat: "pkcs8",
            publicKey: null,
            algorithm: {name: "RSASSA-PKCS1-v1_5"},
            hash: "SHA-256",
            plaintext: plaintext,
            signature: signatures["sha-256"]
        },
        {
            name: "RSASSA-PKCS1-v1_5 with SHA-384",
            publicKeyBuffer: spki,
            publicKeyFormat: "spki",
            privateKey: null,
            privateKeyBuffer: pkcs8,
            privateKeyFormat: "pkcs8",
            publicKey: null,
            algorithm: {name: "RSASSA-PKCS1-v1_5"},
            hash: "SHA-384",
            plaintext: plaintext,
            signature: signatures["sha-384"]
        },
        {
            name: "RSASSA-PKCS1-v1_5 with SHA-512",
            publicKeyBuffer: spki,
            publicKeyFormat: "spki",
            privateKey: null,
            privateKeyBuffer: pkcs8,
            privateKeyFormat: "pkcs8",
            publicKey: null,
            algorithm: {name: "RSASSA-PKCS1-v1_5"},
            hash: "SHA-512",
            plaintext: plaintext,
            signature: signatures["sha-512"]
        }
    ];


    return vectors;
}
