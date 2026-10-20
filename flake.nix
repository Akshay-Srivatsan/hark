{
  description = "Hark, a small synchronous coding agent in C99";

  inputs = {
    nixpkgs.url = "nixpkgs";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, utils, ... }:
    utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        hark = pkgs.stdenv.mkDerivation {
          pname = "hark";
          version = "0.1";

          # Include sources and tests, leaving local build outputs out of the store.
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.fileFilter
              (file: builtins.any file.hasExt [ "c" "h" "sh" "py" ]
                || builtins.elem file.name [ "Makefile" "LICENSE" "THIRD_PARTY_NOTICES.md" ])
              ./.;
          };

          buildInputs = [ pkgs.curl pkgs.tcl ];
          makeFlags = [ "CC=${pkgs.stdenv.cc.targetPrefix}cc" "PREFIX=$(out)" "TCL_LIBS=-ltcl8.6" ];
          enableParallelBuilding = true;

          doCheck = true;
          checkTarget = "test";

          meta = {
            description = "Small synchronous coding agent in C99";
            license = pkgs.lib.licenses.gpl3Plus;
            mainProgram = "hark";
            platforms = pkgs.lib.platforms.unix;
          };
        };
      in {
        packages = {
          inherit hark;
          default = hark;
        };

        checks.hark = hark;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ hark ];
          packages = [ pkgs.python3 ];
        };
      });
}
