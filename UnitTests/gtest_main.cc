// Copyright 2006, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// clang-format off
#include "stdafx.h"
// clang-format on

#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace {

bool try_set_working_directory_near_executable( const char* argv0 ) {
    if( !argv0 || !*argv0 ) {
        return false;
    }

    std::error_code ec;
    fs::path exePath = fs::absolute( argv0, ec );
    if( ec ) {
        return false;
    }

    fs::path dir = exePath.parent_path();

    // Walk upward looking for UnitTests.
    while( true ) {
        const fs::path unitTestsDir = dir / "UnitTests";
        if( fs::is_directory( unitTestsDir, ec ) && !ec ) {
            fs::current_path( unitTestsDir, ec );
            return !ec;
        }

        const fs::path parent = dir.parent_path();
        if( parent == dir ) {
            break;
        }
        dir = parent;
    }

    return false;
}

} // namespace

GTEST_API_ int main( int argc, char** argv ) {
    std::printf( "Running main() from gtest_main.cc\n" );
    std::fflush( stdout );

    if( !try_set_working_directory_near_executable( argc > 0 ? argv[0] : nullptr ) ) {
        std::fprintf( stderr, "Failed to locate UnitTests directory\n" );
        return 1;
    }

    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
