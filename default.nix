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
        pkgs.lib.hasSuffix "/fake_pi" path
      ));
  };
  strictDeps = true;
  nativeBuildInputs = [ pkgs.makeWrapper ];
  doCheck = true;
  checkPhase = "make test";
  installPhase = ''
    make install PREFIX=$out
    wrapProgram "$out/bin/unii" --set UNII_ASSETS "$out/share/unii/pi"
  '';
  meta = {
    description = "One endless chat for pi, backed by a POSIX C89 memory tree";
    license = pkgs.lib.licenses.mit;
    platforms = pkgs.lib.platforms.unix;
  };
}
