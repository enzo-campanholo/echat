{ pkgs ? import <nixpkgs> {} }:
pkgs.stdenv.mkDerivation {
  pname = "uniichat";
  version = "0.1.0";
  src = pkgs.lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      pkgs.lib.cleanSourceFilter path type &&
      !(type == "regular" && (
        pkgs.lib.hasSuffix ".o" path ||
        pkgs.lib.hasSuffix "/unii" path ||
        pkgs.lib.hasSuffix "_test" path ||
        # Generated from pi/prompt.txt: a stale copy would look current here.
        pkgs.lib.hasSuffix "/src/prompt.h" path
      ));
  };
  strictDeps = true;
  doCheck = true;
  checkPhase = "make test";
  installPhase = "make install PREFIX=$out";
  meta = {
    description = "One endless chat for pi, backed by a POSIX C89 memory tree";
    license = pkgs.lib.licenses.mit;
    platforms = pkgs.lib.platforms.unix;
  };
}
