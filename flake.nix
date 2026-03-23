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

        thinkboxlibrary = pkgs.callPackage ./package.nix { };
      in {
        packages = {
          default = thinkboxlibrary;
          thinkboxlibrary = thinkboxlibrary;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ thinkboxlibrary ];

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            clang-tools
            gdb
          ];

          shellHook = ''
            echo "ThinkboxLibrary dev shell"
            echo "Configure with: cmake -S . -B build -G Ninja"
            echo "Build with:     cmake --build build"
            echo "Test with:      ctest --test-dir build"
          '';
        };
      });
}
