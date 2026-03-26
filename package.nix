{ lib
, stdenv
, cmake
, ninja
, pkg-config
, boost
, bzip2
, eigen
, glog
, openexr
, zlib
, tbb
, tinyxml-2
, utf8cpp
, xxHash
, libb2
, icu
, xercesc
, lz4
, libe57format
, gtest
}:

let
  buildUnitTests = true;
  buildWithTbb = true;
  buildWithE57 = false;
  buildWithLz4 = false;
in
stdenv.mkDerivation rec {
  pname = "thinkboxlibrary";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];

  buildInputs =
    [
      boost
      bzip2
      eigen
      glog
      openexr
      zlib
      tinyxml-2
      utf8cpp
      xxHash
      libb2
    ]
    ++ lib.optionals buildWithTbb [
      tbb
    ]
    ++ lib.optionals stdenv.hostPlatform.isLinux [
      icu
    ]
    ++ lib.optionals buildWithE57 [
      xercesc
      libe57format
    ]
    ++ lib.optionals buildWithLz4 [
      lz4
    ]
    ++ lib.optionals buildUnitTests [
      gtest
    ];

  cmakeFlags = [
    "-DBUILD_UNIT_TESTS=${if buildUnitTests then "ON" else "OFF"}"
    "-DBUILD_WITH_TBB=${if buildWithTbb then "ON" else "OFF"}"
    "-DBUILD_WITH_E57=${if buildWithE57 then "ON" else "OFF"}"
    "-DBUILD_WITH_LZ4=${if buildWithLz4 then "ON" else "OFF"}"
  ] ++ lib.optionals buildUnitTests [
    "-DGTEST_ROOT=${gtest}"
  ];

  # Useful if upstream code assumes Release-like builds.
  cmakeBuildType = "Release";

  doCheck = false;

  meta = with lib; {
    description = "Thinkbox core C++ library";
    license = licenses.asl20;
    platforms = platforms.unix;
  };
}
