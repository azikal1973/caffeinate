# Paketbeschreibung fuer Nix/NixOS. Mit Flakes siehe flake.nix, ohne:
#   nix-build -E 'with import <nixpkgs> { }; callPackage ./package.nix { }'
{ lib, stdenv }:

stdenv.mkDerivation {
  pname = "caffeinate";
  version = "0.2.0.0";

  # nur die Quellen, kein evtl. schon gebautes Binary aus dem Arbeitsverzeichnis
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./caffeinate.c
      ./caffeinate.8
      ./tests
    ];
  };

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  # Der D-Bus-Test braucht dbus-daemon und PyGObject; im Build genuegt der Smoke-Test.
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
