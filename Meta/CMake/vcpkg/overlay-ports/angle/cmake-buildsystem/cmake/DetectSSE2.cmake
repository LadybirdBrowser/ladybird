# https://github.com/WebKit/WebKit/blob/0742522b24152262b04913242cb0b3c48de92ba0/Source/cmake/DetectSSE2.cmake

#################################
# Check for the presence of SSE2.
#
# Once done, this will define:
# - SSE2_SUPPORT_FOUND - the system supports (at least) SSE2.
#
# Copyright (c) 2014, Pablo Fernandez Alcantarilla, Jesus Nuevo
# Copyright (c) 2019, Igalia S.L.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
#   * Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#   * Redistributions in binary form must reproduce the above copyright notice,
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#   * Neither the name of the copyright holders nor the names of its contributors
#     may be used to endorse or promote products derived from this software without
#     specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
# SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
# TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
# BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
# WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set(SSE2_SUPPORT_FOUND FALSE)

macro(CHECK_FOR_SSE2)
    include(CheckCXXSourceRuns)

    check_cxx_source_runs("
        #include <emmintrin.h>
        int main ()
        {
            __m128d a, b;
            double vals[2] = {0};
            a = _mm_loadu_pd (vals);
            b = _mm_add_pd (a,a);
            _mm_storeu_pd (vals,b);
            return 0;
        }"
            HAVE_SSE2_EXTENSIONS)

    if (COMPILER_IS_GCC_OR_CLANG OR (MSVC AND NOT CMAKE_CL_64))
        if (HAVE_SSE2_EXTENSIONS)
            set(SSE2_SUPPORT_FOUND TRUE)
            message(STATUS "Found SSE2 extensions")
        endif ()
    endif ()

endmacro(CHECK_FOR_SSE2)

CHECK_FOR_SSE2()