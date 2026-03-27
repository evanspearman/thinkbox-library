{
  description = "ThinkboxLibrary";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
        };

        llvm = pkgs.llvmPackages_latest;
        thinkboxlibrary = pkgs.callPackage ./package.nix {
          stdenv = pkgs.clangStdenv;
          src = self;
        };
      in {
        packages = {
          default = thinkboxlibrary;
          thinkboxlibrary = thinkboxlibrary;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ thinkboxlibrary ];

          packages = [
            llvm.clang
            llvm.lld
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.clang-tools
            pkgs.just
          ];

          shellHook = ''
            export CC="${llvm.clang}/bin/clang"
            export CXX="${llvm.clang}/bin/clang++"
            export CXXFLAGS="-fcolor-diagnostics"
          '';
        };
      });
}
