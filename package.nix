# Package definition for Nix/NixOS. With flakes see flake.nix; without:
#   nix-build -E 'with import <nixpkgs> { }; callPackage ./package.nix { }'
{ lib, stdenv }:

stdenv.mkDerivation {
  pname = "caffeinate";
  version = "0.3.0.0";

  # only the sources, not a binary that may already sit in the working directory
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./caffeinate.c
      ./caffeinate.h
      ./caffeinate.8
      ./tests
    ];
  };

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  # The D-Bus test needs dbus-daemon and PyGObject; for the build the smoke test is enough.
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    sh tests/smoke.sh ./caffeinate
    runHook postCheck
  '';

  meta = {
    description = "Nachbau von macOS caffeinate(8): verhindert Ruhezustand und Bildschirmschoner";
    homepage = "https://github.com/azikal1973/caffeinate";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "caffeinate";
  };
}
